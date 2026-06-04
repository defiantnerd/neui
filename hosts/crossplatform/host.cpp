#include <vector>
#include <memory>
#include <cstring>
#include <algorithm>

#include "host.h"
#include "platform.h"
#include "../shared/widget_paint_knob.h"
#include "../shared/widget_paint_section.h"
#include "../shared/widget_paint_compound.h"
#include "../shared/widget_paint_grid.h"
#include "../shared/theme_palette.h"
#include "../shared/painter.h"
#include "../shared/widget_font.h"
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

  // Invalidate the widget's owning frame if the widget hosts a CUSTOMDRAW
  // compound whose layers depend on state (NEUI_LAYER_STATE_* via show_when).
  // Called from set_hovered / set_pressed on each side of a transition so
  // the compound repaints to swap state-filtered layers in / out.
  static void invalidate_if_state_filtered_compound(Session* s, uint32_t widget_idx)
  {
    if (!s || widget_idx == 0 || !s->_widgets.exists(widget_idx)) return;
    auto& wd = s->_widgets[widget_idx];
    auto* cd = dynamic_cast<CustomDrawWidget*>(&wd);
    if (!cd) return;
    auto* ca = resolve_widget_compound(s, cd->compound_asset);
    if (!ca) return;
    if (!neui_detail::compound_has_state_filters(*ca)) return;
    void* frame = s->find_parent_native_handle(widget_idx);
    if (frame) platform_invalidate(frame);
  }

  // ---------------------------------------------------------------------------
  // UTF-8 helpers (byte-string cursor navigation)

  // Byte length of the UTF-8 character that starts at text[pos].
  // Returns 1 for invalid / continuation bytes so the cursor always advances.
  static int utf8_char_len(const std::string& s, int pos)
  {
    if (pos < 0 || pos >= static_cast<int>(s.size())) return 0;
    unsigned char c = static_cast<unsigned char>(s[pos]);
    if (c < 0x80)           return 1;
    if ((c & 0xE0) == 0xC0) return 2;
    if ((c & 0xF0) == 0xE0) return 3;
    if ((c & 0xF8) == 0xF0) return 4;
    return 1;  // continuation byte or invalid
  }

  // Byte offset of the start of the UTF-8 character that precedes pos.
  static int utf8_prev_start(const std::string& s, int pos)
  {
    if (pos <= 0) return 0;
    --pos;
    while (pos > 0 && (static_cast<unsigned char>(s[pos]) & 0xC0) == 0x80)
      --pos;
    return pos;
  }

  // Forward declarations for word-navigation helpers used by InputBoxWidget
  // and MultilineWidget keydown handlers; bodies are below near find_word_bounds.
  static int word_left (const std::string& s, int pos);
  static int word_right(const std::string& s, int pos);

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
      uint8_t a = attrs[run_start];
      int run_end = run_start + 1;
      while (run_end < n && attrs[run_end] == a) ++run_end;

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
  extern neui_grid_api_t      grid_api;

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
    if (!strcmp(iface, NEUI_API_GRID))      return &grid_api;
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

  // -------------------------------------------------------------------------
  // Session implementation

  Session::Session(neui_client_t* client, void* token)
    : _client(client), _token(token)
  {
    _client_widget_api = static_cast<neui_widget_client_t*>(
      _client->get_interface(token, NEUI_API_WIDGETS));
    _backend = platform_get_backend();

    // Opt-in clipboard-change notifications: the client implements
    // neui_clipboard_client_t and exposes it via its get_interface.
    _clipboard_client = static_cast<neui_clipboard_client_t*>(
      _client->get_interface(token, NEUI_API_CLIPBOARD_CLIENT));
    if (_clipboard_client && _clipboard_client->onchange) {
      _clipboard_listener_handle = platform_register_clipboard_listener(
        [](void* tok) {
          auto* self = static_cast<Session*>(tok);
          if (self && self->_clipboard_client && self->_clipboard_client->onchange)
            self->_clipboard_client->onchange(self->_token);
        },
        this);
    }

    // Opt-in menu-item validation callback. Polled per item at popup-open.
    _menu_client = static_cast<neui_menu_client_t*>(
      _client->get_interface(token, NEUI_API_MENU_CLIENT));

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
    if (_clipboard_listener_handle != 0) {
      platform_unregister_clipboard_listener(_clipboard_listener_handle);
      _clipboard_listener_handle = 0;
    }
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

  bool Session::dispatch_event(neui_event_t* event)
  {
    if (_client_widget_api && _client_widget_api->onevent)
      return _client_widget_api->onevent(_token, event);
    return false;
  }

  bool Session::dispatch_menu_event(uint32_t cmd_id)
  {
    for (uint32_t mb_idx : _menubars) {
      if (!_widgets.exists(mb_idx)) continue;
      auto& mb = dynamic_cast<MenubarWidget&>(_widgets[mb_idx]);
      auto it = mb.menu_cmd_map.find(cmd_id);
      if (it == mb.menu_cmd_map.end()) continue;
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
      return dispatch_event(&ev);
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
        if (!wd.native_handle && !wd.is_menubar() && wd.visible) {
          if (wd.hit_test(x, y)) {
            // Disabled widgets are click-transparent: they don't claim
            // the hit, but children remain hit-testable. This matches
            // Win32 EnableWindow semantics, where a disabled control
            // passes clicks through to its parent / siblings.
            if (wd.emit_events && wd.enabled)
              result = idx;
            uint32_t deeper = widget_at_recursive(widgets, idx, x, y);
            if (deeper != 0) result = deeper;
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
      if (wd.emit_events) {
        neui_event_t ev = {};
        ev.type = NEUI_EVENT_WIDGET_FOCUS;
        ev.data.focus.widget  = { wd.widget_id };
        ev.data.focus.focused = true;
        dispatch_event(&ev);
      }
    }

    uint32_t ref = (new_idx != 0) ? new_idx : _focused_widget;
    void* frame = find_parent_native_handle(ref);
    if (frame) platform_invalidate(frame);
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

    invalidate_if_state_filtered_compound(this, old_idx);
    invalidate_if_state_filtered_compound(this, new_idx);
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

    invalidate_if_state_filtered_compound(this, old_idx);
    invalidate_if_state_filtered_compound(this, new_idx);
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

  // SECTION - non-interactive visual container. Body filled with the
  // section colour (NEUI_ATTR_BACKGROUND override, else a theme-derived
  // shade lighter than frame_bg so it reads as a raised panel). Optional
  // `text` is drawn as a header in a top band, where only a tight title
  // chip is filled with the section colour - the rest of the band is left
  // unpainted so the frame's earlier `begin_frame` clear shows through,
  // giving the chip a "tab" look. Geometry + paint live in the shared
  // helper so the win32 host renders identically.
  void SectionWidget::paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
                             bool /*is_focused*/)
  {
    using neui_detail::ColorRole;
    uint32_t bg = neui_detail::shade(
                    neui_detail::color(ColorRole::frame_bg),
                    neui_detail::SECTION_BG_LIFT);
    if (attrs && attrs->has(NEUI_ATTR_BACKGROUND))
      bg = static_cast<uint32_t>(attrs->get_int(NEUI_ATTR_BACKGROUND, 0));
    const char* align = attrs ? attrs->get_string(NEUI_ATTR_ALIGN_TEXT) : nullptr;
    neui_detail::paint_section(backend, ctx,
                                static_cast<float>(x),
                                static_cast<float>(y),
                                static_cast<float>(width),
                                static_cast<float>(height),
                                text.c_str(), bg, align,
                                neui_detail::color(ColorRole::text_primary),
                                attrs.get());
  }

  void ButtonWidget::paint(neui_render_backend_t* backend, neui_render_ctx_t ctx,
                            bool is_focused)
  {
    using neui_detail::ColorRole;
    float fx = static_cast<float>(x);
    float fy = static_cast<float>(y);
    float fw = static_cast<float>(width);
    float fh = static_cast<float>(height);
    backend->fill_rect(ctx, fx, fy, fw, fh,
                       neui_detail::color(ColorRole::panel_bg));
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
    if (sel_anchor != cursor_pos) {
      int lo = std::min(cursor_pos, sel_anchor);
      int hi = std::max(cursor_pos, sel_anchor);
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
        int char_end = cursor_pos + utf8_char_len(text, cursor_pos);
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
      composition_text.assign(utf8 ? utf8 : "", byte_len > 0 ? byte_len : 0);
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
      bool has_sel = (sel_anchor != cursor_pos);
      // History entry uses the pre-composition snapshot so undo restores the
      // state from before the user even started composing.
      history.mark(composition_pre_state,
                   neui_detail::EditHistory::Typing, has_sel);
      if (has_sel) {
        int lo = std::min(cursor_pos, sel_anchor);
        int hi = std::max(cursor_pos, sel_anchor);
        text.erase(lo, hi - lo);
        cursor_pos = sel_anchor = lo;
      }
      if (utf8 && byte_len > 0) {
        text.insert(cursor_pos, utf8, byte_len);
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

  bool InputBoxWidget::on_keydown(uint32_t keycode, uint32_t modifiers)
  {
    int text_len = static_cast<int>(text.size());
    bool shift   = (modifiers & 1) != 0;
    bool ctrl    = (modifiers & 2) != 0;

    auto has_sel  = [&]{ return sel_anchor != cursor_pos; };
    auto sel_lo   = [&]{ return std::min(cursor_pos, sel_anchor); };
    auto sel_hi   = [&]{ return std::max(cursor_pos, sel_anchor); };
    auto collapse = [&]{ sel_anchor = cursor_pos; };
    auto del_sel  = [&]{
      int lo = sel_lo(), hi = sel_hi();
      text.erase(lo, hi - lo);
      cursor_pos = sel_anchor = lo;
    };
    auto snapshot = [&]() -> neui_detail::EditState {
      return { text, cursor_pos, sel_anchor };
    };

    switch (keycode) {
    case NEUI_KEY_LEFT:
      history.reset_action();
      if (!shift && !ctrl && has_sel()) {
        cursor_pos = sel_lo(); collapse();
      } else if (ctrl) {
        cursor_pos = word_left(text, cursor_pos);
        if (!shift) collapse();
      } else {
        if (cursor_pos > 0)
          cursor_pos = utf8_prev_start(text, cursor_pos);
        if (!shift) collapse();
      }
      repaint(); return true;
    case NEUI_KEY_RIGHT:
      history.reset_action();
      if (!shift && !ctrl && has_sel()) {
        cursor_pos = sel_hi(); collapse();
      } else if (ctrl) {
        cursor_pos = word_right(text, cursor_pos);
        if (!shift) collapse();
      } else {
        if (cursor_pos < text_len)
          cursor_pos += utf8_char_len(text, cursor_pos);
        if (!shift) collapse();
      }
      repaint(); return true;
    case NEUI_KEY_HOME:
      history.reset_action();
      cursor_pos = 0;
      if (!shift) collapse();
      repaint(); return true;
    case NEUI_KEY_END:
      history.reset_action();
      cursor_pos = text_len;
      if (!shift) collapse();
      repaint(); return true;
    case NEUI_KEY_INSERT:
      history.reset_action();
      overwrite_mode = !overwrite_mode;
      repaint(); return true;
    case NEUI_KEY_BACK:
      if (has_sel() || cursor_pos > 0) {
        history.mark(snapshot(), neui_detail::EditHistory::Deleting, has_sel());
        if (has_sel()) {
          del_sel();
        } else {
          int start = ctrl ? word_left(text, cursor_pos)
                           : utf8_prev_start(text, cursor_pos);
          text.erase(start, cursor_pos - start);
          cursor_pos = sel_anchor = start;
        }
      }
      repaint(); return true;
    case NEUI_KEY_DELETE:
      if (has_sel() || cursor_pos < text_len) {
        history.mark(snapshot(), neui_detail::EditHistory::Deleting, has_sel());
        if (has_sel()) {
          del_sel();
        } else {
          int end = ctrl ? word_right(text, cursor_pos)
                         : cursor_pos + utf8_char_len(text, cursor_pos);
          text.erase(cursor_pos, end - cursor_pos);
        }
      }
      repaint(); return true;
    case NEUI_KEY_A:
      if (ctrl) {
        history.reset_action();
        cursor_pos = text_len;
        sel_anchor = 0;
        repaint();
        return true;
      }
      break;
    case NEUI_KEY_C:
      if (ctrl) {
        if (has_sel()) {
          int lo = sel_lo(), hi = sel_hi();
          platform_clipboard_set_text(
            text.c_str() + lo, static_cast<uint32_t>(hi - lo));
        }
        return true;
      }
      break;
    case NEUI_KEY_X:
      if (ctrl) {
        bool readonly = attrs && attrs->get_int(NEUI_ATTR_READONLY, 0) != 0;
        if (has_sel()) {
          int lo = sel_lo(), hi = sel_hi();
          platform_clipboard_set_text(
            text.c_str() + lo, static_cast<uint32_t>(hi - lo));
          if (!readonly) {
            history.mark(snapshot(), neui_detail::EditHistory::None, true);
            del_sel();
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
        if (n <= 0) return true;
        std::vector<char> buf(static_cast<size_t>(n));
        platform_clipboard_get_text(buf.data(), n);
        // Drop trailing null and any embedded \r/\n (single-line input).
        std::string paste;
        paste.reserve(buf.size());
        for (size_t i = 0; i + 1 < buf.size(); ++i) {
          char c = buf[i];
          if (c == '\r' || c == '\n') continue;
          paste.push_back(c);
        }
        history.mark(snapshot(), neui_detail::EditHistory::None, has_sel());
        if (has_sel()) del_sel();
        text.insert(cursor_pos, paste);
        cursor_pos += static_cast<int>(paste.size());
        sel_anchor = cursor_pos;
        repaint();
        return true;
      }
      break;
    case NEUI_KEY_Z:
      if (ctrl) {
        neui_detail::EditState restored;
        bool ok = shift ? history.redo(snapshot(), restored)
                        : history.undo(snapshot(), restored);
        if (ok) {
          text       = restored.text;
          cursor_pos = restored.cursor;
          sel_anchor = restored.anchor;
          repaint();
        }
        return true;
      }
      break;
    case NEUI_KEY_Y:
      if (ctrl) {
        neui_detail::EditState restored;
        if (history.redo(snapshot(), restored)) {
          text       = restored.text;
          cursor_pos = restored.cursor;
          sel_anchor = restored.anchor;
          repaint();
        }
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

  bool InputBoxWidget::on_keychar(uint32_t codepoint, uint32_t modifiers)
  {
    if (codepoint < 0x20 || codepoint == 0x7F) return false;

    int text_len = static_cast<int>(text.size());

    // Encode the Unicode codepoint to UTF-8.
    char utf8[5] = {};
    int  byte_count = 0;
    if (codepoint < 0x80) {
      utf8[0] = static_cast<char>(codepoint);
      byte_count = 1;
    } else if (codepoint < 0x800) {
      utf8[0] = static_cast<char>(0xC0 | (codepoint >> 6));
      utf8[1] = static_cast<char>(0x80 | (codepoint & 0x3F));
      byte_count = 2;
    } else if (codepoint < 0x10000) {
      utf8[0] = static_cast<char>(0xE0 | (codepoint >> 12));
      utf8[1] = static_cast<char>(0x80 | ((codepoint >> 6) & 0x3F));
      utf8[2] = static_cast<char>(0x80 | (codepoint & 0x3F));
      byte_count = 3;
    } else {
      utf8[0] = static_cast<char>(0xF0 | (codepoint >> 18));
      utf8[1] = static_cast<char>(0x80 | ((codepoint >> 12) & 0x3F));
      utf8[2] = static_cast<char>(0x80 | ((codepoint >>  6) & 0x3F));
      utf8[3] = static_cast<char>(0x80 | (codepoint & 0x3F));
      byte_count = 4;
    }

    bool has_sel = (sel_anchor != cursor_pos);
    history.mark(neui_detail::EditState{ text, cursor_pos, sel_anchor },
                 neui_detail::EditHistory::Typing, has_sel);
    if (has_sel) {
      int lo = std::min(cursor_pos, sel_anchor);
      int hi = std::max(cursor_pos, sel_anchor);
      text.erase(lo, hi - lo);
      cursor_pos = sel_anchor = lo;
      text.insert(cursor_pos, utf8, byte_count);
    } else if (overwrite_mode && cursor_pos < text_len) {
      int existing_len = utf8_char_len(text, cursor_pos);
      text.replace(cursor_pos, existing_len, utf8, byte_count);
    } else {
      text.insert(cursor_pos, utf8, byte_count);
    }
    cursor_pos += byte_count;
    sel_anchor  = cursor_pos;

    if (session) {
      void* frame = session->find_parent_native_handle(index);
      if (frame) platform_invalidate(frame);
    }
    return true;
  }

  // A "word character" is alphanumeric ASCII or any byte with the high bit set
  // (lead/continuation bytes of a UTF-8 multi-byte sequence - treated as part
  // of a word so simple Latin-extended/CJK selection works without locale data).
  static bool is_word_byte(unsigned char c)
  {
    return (c >= '0' && c <= '9') ||
           (c >= 'A' && c <= 'Z') ||
           (c >= 'a' && c <= 'z') ||
           c == '_' || c >= 0x80;
  }

  // Forward word navigation: returns the next word-start at or after pos.
  // A "word-start" is a position where is_word_byte(s[pos]) is true and
  // is_word_byte(s[pos-1]) is false (or pos == 0). Lands at end-of-text if
  // there is no further word. Whitespace, punctuation, and '\n' all count as
  // non-word, so this naturally crosses line boundaries in multiline text.
  static int word_right(const std::string& s, int pos)
  {
    int len = static_cast<int>(s.size());
    if (pos >= len) return len;
    ++pos;  // step at least once so repeated invocations make progress
    while (pos < len) {
      bool here = is_word_byte(static_cast<unsigned char>(s[pos]));
      bool prev = is_word_byte(static_cast<unsigned char>(s[pos - 1]));
      if (here && !prev) break;
      ++pos;
    }
    return pos;
  }

  // Backward word navigation: returns the previous word-start before pos.
  // Lands at 0 if there is no earlier word.
  static int word_left(const std::string& s, int pos)
  {
    if (pos <= 0) return 0;
    --pos;  // step at least once
    while (pos > 0) {
      bool here = is_word_byte(static_cast<unsigned char>(s[pos]));
      bool prev = is_word_byte(static_cast<unsigned char>(s[pos - 1]));
      if (here && !prev) break;
      --pos;
    }
    return pos;
  }

  // Returns [start, end) of the contiguous run of same-class bytes containing
  // `pos`. If pos sits at the boundary at end-of-text, returns [pos, pos].
  static void find_word_bounds(const std::string& s, int pos, int& start, int& end)
  {
    int len = static_cast<int>(s.size());
    if (pos < 0) pos = 0;
    if (pos > len) pos = len;
    if (len == 0) { start = end = 0; return; }

    // If pos is at the end of text, fall back to expanding from the previous byte.
    int probe = (pos < len) ? pos : pos - 1;
    bool word = is_word_byte(static_cast<unsigned char>(s[probe]));

    start = pos;
    while (start > 0 &&
           is_word_byte(static_cast<unsigned char>(s[start - 1])) == word)
      --start;

    end = (pos < len) ? pos : len;
    while (end < len &&
           is_word_byte(static_cast<unsigned char>(s[end])) == word)
      ++end;
  }

  bool InputBoxWidget::on_mouse_event(neui_event_t* event)
  {
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
        int char_end = pos + utf8_char_len(text, pos);
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
      find_word_bounds(text, new_pos, ws, we);
      sel_anchor = ws;
      cursor_pos = we;
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
                           dst_x, dst_y, dst_w, dst_h);
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
      for (auto& kv : menu_items) {
        if (kv.second.submenu)
          platform_menubar_destroy(kv.second.submenu);
      }
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
        if (!wd.native_handle && !wd.is_menubar() && wd.visible && wd.width > 0 && wd.height > 0) {
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
        // Translate so the child's descendants - which store coords
        // relative to the child - draw at the correct absolute position.
        if (backend->push_transform) backend->push_transform(ctx);
        if (backend->translate)
          backend->translate(ctx, static_cast<float>(wd.x), static_cast<float>(wd.y));
        paint_widgets_recursive(backend, ctx, widgets, idx,
                                wd.abs_x, wd.abs_y, focused_widget);
        // After-children hook: widget-local coords are active here (we
        // translated by (wd.x, wd.y) above and have not popped yet).
        // Used by CUSTOMDRAW + compound to paint z>=0 layers above the
        // child-widget pass; default implementation is a no-op.
        if (!wd.native_handle && !wd.is_menubar() && wd.visible && wd.width > 0 && wd.height > 0) {
          wd.paint_after_children(backend, ctx, idx == focused_widget);
        }
        if (backend->pop_transform) backend->pop_transform(ctx);
      }
      idx = widgets.next(idx);
    }
  }

  void Session::paint_frame(neui_render_ctx_t ctx, uint32_t parent_index)
  {
    if (!_backend || !ctx) return;
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
    if (parent_index < UINT32_MAX) {
      WidgetData* fw = get_widget(parent_index);
      if (fw && fw->attrs && fw->attrs->has(NEUI_ATTR_BACKGROUND))
        clear = static_cast<uint32_t>(fw->attrs->get_int(NEUI_ATTR_BACKGROUND, 0));
    }
    _backend->begin_frame(ctx, clear);
    // While the frame doesn't own OS keyboard focus, suppress focus
    // decorations (caret, focus outline) by reporting "no focused widget" to
    // the painters. The logical focus is preserved so input routing snaps
    // back when the frame regains OS focus.
    uint32_t focus_for_paint = _os_focused ? _focused_widget : 0;
    // Frame is the root of the absolute coord space - start the walk at
    // (0, 0). The frame's own widget rect isn't painted by this walk
    // (the begin_frame above did the clear); recursion enters its
    // children directly.
    paint_widgets_recursive(_backend, ctx, _widgets, parent_index, 0, 0, focus_for_paint);
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
    // Popup-menu overlay sits on top of the combo overlay.
    paint_popup_menu(ctx);
    _backend->end_frame(ctx);

    // Restore the previous override so non-paint callers (event
    // handlers, theme provider, other frames painted in the same pump
    // iteration) see the session's default tracking palette again.
    neui_detail::set_active_palette_override(prev_override);
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
        if (!wd.native_handle && !wd.is_menubar() && wd.visible && wd.tab_stop && wd.enabled)
          out.push_back(idx);
        collect_tab_stops(widgets, idx, out);
      }
      idx = widgets.next(idx);
    }
  }

  void Session::focus_next(bool forward)
  {
    std::vector<uint32_t> stops;
    uint32_t root_child = _widgets.child(0);
    while (root_child != 0) {
      if (_widgets.exists(root_child))
        collect_tab_stops(_widgets, root_child, stops);
      root_child = _widgets.next(root_child);
    }

    if (stops.empty()) return;

    int cur = -1;
    for (int i = 0; i < static_cast<int>(stops.size()); ++i) {
      if (stops[i] == _focused_widget) { cur = i; break; }
    }

    int next;
    if (cur < 0) {
      next = forward ? 0 : static_cast<int>(stops.size()) - 1;
    } else {
      int delta = forward ? 1 : -1;
      next = (cur + delta + static_cast<int>(stops.size()))
             % static_cast<int>(stops.size());
    }

    set_focus(stops[next]);
  }

  // -------------------------------------------------------------------------
  // Mouse dispatch

  void Session::dispatch_mouse_event(uint32_t widget_idx, neui_event_t* ev)
  {
    if (widget_idx == 0 || !_widgets.exists(widget_idx)) return;
    auto& w = _widgets[widget_idx];
    if (!w.emit_events) return;
    if (!w.enabled) return;  // disabled widgets don't receive mouse events
    if (!dispatch_event(ev))
      w.on_mouse_event(ev);
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

  static void widget_set_value_silent(WidgetData& wd, float v)
  {
    v = snap_to_steps(clamp01(v), widget_get_steps(wd));
    neui_detail::ensure_attrs(wd.attrs).set_float(NEUI_PARAM_VALUE, v);
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
      widget_set_value_user(*this, v);
      repaint();
    }
    return handled;
  }

  bool SliderWidget::on_mouse_event(neui_event_t* event)
  {
    slider_resolve_orientation(*this);

    if (event->type == NEUI_EVENT_MOUSE_BUTTON_DBLCLICK) {
      // Reset to NEUI_PARAM_DEFAULT (or 0 if unset). The preceding
      // MOUSE_BUTTON_DOWN already started a drag and snapped to the click
      // position; cancel that drag and overwrite with the default.
      float def = 0.0f;
      if (attrs) def = clamp01(attrs->get_float(NEUI_PARAM_DEFAULT, 0.0f));
      widget_set_value_user(*this, def);
      dragging = false;
      repaint();
      return true;
    }

    if (event->type == NEUI_EVENT_MOUSE_WHEEL) {
      int delta = event->data.wheel.delta;
      bool fine = (event->data.mouse.buttonmap & NEUI_MK_SHIFT) != 0;
      float step = nudge_delta(*this, 1, fine ? 0.01f : 0.05f) *
                   (delta > 0 ? 1.0f : -1.0f);
      widget_set_value_user(*this, widget_get_value(*this) + step);
      repaint();
      return true;
    }

    if (dragging) {
      if (event->type == NEUI_EVENT_MOUSE_BUTTON_UP ||
          (event->type == NEUI_EVENT_MOUSE_MOVE &&
            !(event->data.mouse.buttonmap & NEUI_MK_LBUTTON))) {
        dragging = false;
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
      // Always jump to clicked position, then start drag.
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
  // exposing the raw bitmap pointer to clients.
  static void NEUI_ABI xpl_painter_draw_asset_thunk(
      void* host_token,
      neui_render_backend_t* backend,
      neui_render_ctx_t ctx,
      neui_asset_t asset,
      float x, float y, float w, float h)
  {
    auto* s = static_cast<Session*>(host_token);
    if (!s || !backend || !ctx) return;
    if (asset.id == asset_none.id) return;
    // Reject cross-session handles.
    if (((asset.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return;
    uint32_t slot = asset.id & 0xffff;
    auto* entry = s->_asset_manager.get_slot(slot);
    if (!entry) return;
    // Lazy GPU upload for this (asset, ctx) pair, with device-loss check.
    // If the backend has bumped its per-ctx generation (D2D after
    // D2DERR_RECREATE_TARGET) any cached handle is dangling - drop it
    // and re-upload against the new target.
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
      if (!backend->create_bitmap) return;
      void* bmp = backend->create_bitmap(ctx,
                                          entry->width_px, entry->height_px,
                                          entry->pixels.data(),
                                          entry->scale);
      if (!bmp) return;
      it = entry->bitmaps.emplace(ctx,
                                   neui_detail::CtxBitmap{ bmp, gen }).first;
    }
    if (backend->draw_bitmap)
      backend->draw_bitmap(ctx, it->second.bmp,
                            0.0f, 0.0f, 0.0f, 0.0f,    // full bitmap
                            x, y, w, h);
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

    if (backend->push_transform) backend->push_transform(ctx);
    if (backend->translate)
      backend->translate(ctx, static_cast<float>(x), static_cast<float>(y));
    if (backend->push_clip) backend->push_clip(ctx, 0.0f, 0.0f, fw, fh);

    neui_painter painter{};
    painter.backend          = backend;
    painter.ctx              = ctx;
    painter.host_token       = session;
    painter.draw_asset_thunk = &xpl_painter_draw_asset_thunk;

    if (auto* ca = resolve_widget_compound(session, compound_asset)) {
      // Compound mode: paint z<0 layers here; z>=0 layers come from
      // paint_after_children below. WIDGET_PAINT is suppressed.
      uint32_t state_mask = neui_detail::compose_widget_state(enabled, hovered, pressed);
      neui_detail::paint_compound_below(&painter, *ca, fw, fh,
                                          neui_detail::attrs_readonly(attrs),
                                          state_mask);
    } else {
      neui_event_t ev{};
      ev.type = NEUI_EVENT_WIDGET_PAINT;
      ev.data.paint.widget.id  = widget_id;
      ev.data.paint.painter_api = &neui_detail::k_painter_api;
      ev.data.paint.p           = &painter;
      ev.data.paint.width       = fw;
      ev.data.paint.height      = fh;
      ev.data.paint.focused     = is_focused;
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

    uint32_t state_mask = neui_detail::compose_widget_state(enabled, hovered, pressed);
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

  static int xpl_behavior_popup_menu(void* host_data, int local_x, int local_y,
                                       const char* const* items)
  {
    auto* wd = static_cast<CustomDrawWidget*>(host_data);
    if (!wd || !wd->session || !items) return 0;
    std::vector<std::string> v;
    for (int i = 0; items[i] != nullptr; ++i) v.emplace_back(items[i]);
    return wd->session->open_popup_menu(wd->index, local_x, local_y, v);
  }

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
    ctx.popup_menu        = &xpl_behavior_popup_menu;
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
      widget_set_value_user(*this, v);
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
      bool fine = (event->data.mouse.buttonmap & NEUI_MK_SHIFT) != 0;
      // Wheel up DECREASES knob value, wheel down INCREASES - matches
      // audio-plugin convention (scroll-up "lifts" away from you, value
      // drops; scroll-down "pulls" toward you, value rises).
      float step = nudge_delta(*this, 1, fine ? 0.01f : 0.05f) *
                   (delta > 0 ? -1.0f : 1.0f);
      widget_set_value_user(*this, widget_get_value(*this) + step);
      repaint();
      return true;
    }

    if (event->type == NEUI_EVENT_MOUSE_BUTTON_DBLCLICK) {
      float def = 0.0f;
      if (attrs) def = clamp01(attrs->get_float(NEUI_PARAM_DEFAULT, 0.0f));
      widget_set_value_user(*this, def);
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
        widget_set_value_user(*this, def);
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
      return true;
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // ListItemsWidget (LISTBOX) and ComboBoxWidget (COMBOBOX)

  static constexpr int LIST_ITEM_H       = 18;
  static constexpr int COMBO_COLLAPSED_H = 22;   // height of the collapsed combo bar
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
    return height / LIST_ITEM_H;
  }

  // Shared rendering helper for any scrollable item list.
  // fx/fy/fw/fh define the total rect (including scrollbar column when shown).
  // full_vis: floor-visible rows - used only for scrollbar sizing; pass
  //           (int_height / LIST_ITEM_H) for listbox, max_drop_visible() for overlay.
  // scroll_offset is read-only here; callers own the value.
  // border_color 0 = no outer border drawn.
  static void paint_scrollable_list(
      neui_render_backend_t* backend, neui_render_ctx_t ctx,
      float fx, float fy, float fw, float fh,
      const std::vector<ListItemsWidget::Item>& items,
      uint32_t selected_item, uint32_t scroll_offset,
      int full_vis, int int_h,
      uint32_t bg_color, uint32_t border_color,
      float    text_size = 12.0f)
  {
    uint32_t n       = static_cast<uint32_t>(items.size());
    bool     show_sb = n > static_cast<uint32_t>(full_vis);
    float    content_w = show_sb ? fw - static_cast<float>(SCROLLBAR_W) : fw;

    backend->fill_rect(ctx, fx, fy, content_w, fh, bg_color);

    // Ceiling division: draw a partial row at the bottom if it fits.
    int max_visible = (int_h + LIST_ITEM_H - 1) / LIST_ITEM_H;
    if (max_visible < 1) max_visible = 1;

    using neui_detail::ColorRole;
    uint32_t accent_col      = neui_detail::color(ColorRole::accent);
    uint32_t selected_text   = neui_detail::color(ColorRole::accent_text);
    uint32_t normal_text     = neui_detail::color(ColorRole::text_primary);

    if (backend->push_clip) backend->push_clip(ctx, fx, fy, content_w, fh);
    for (int i = 0; i < max_visible; ++i) {
      uint32_t item_idx = scroll_offset + static_cast<uint32_t>(i);
      if (item_idx >= n) break;
      float row_y = fy + static_cast<float>(i * LIST_ITEM_H);
      bool sel = (item_idx == selected_item);
      if (sel)
        backend->fill_rect(ctx, fx + 1.0f, row_y, content_w - 2.0f,
                           static_cast<float>(LIST_ITEM_H), accent_col);
      if (backend->draw_text)
        backend->draw_text(ctx, fx + 4.0f, row_y, content_w - 8.0f,
                           static_cast<float>(LIST_ITEM_H),
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
        height / LIST_ITEM_H, height,
        neui_detail::color(ColorRole::control_bg), border_color,
        ef.size);
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
                      + static_cast<int>(delta_y * static_cast<float>(scroll_range)
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
    uint32_t clicked = scroll_offset + static_cast<uint32_t>(rel_y / LIST_ITEM_H);
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
    // Only the top bar (collapsed height) is interactive; the rest is the
    // drop area. Bounds are in frame-local coords (same space as px/py).
    return px >= abs_x && px < abs_x + width &&
           py >= abs_y && py < abs_y + COMBO_COLLAPSED_H;
  }

  int ComboBoxWidget::max_drop_visible() const
  {
    int avail = height - COMBO_COLLAPSED_H;
    return (avail > 0) ? avail / LIST_ITEM_H : 0;
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
    float fh = static_cast<float>(COMBO_COLLAPSED_H);   // only the top bar
    const float arrow_w = 18.0f;

    backend->fill_rect(ctx, fx, fy, fw, fh,
                        neui_detail::color(ColorRole::control_bg));

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
    float ox = static_cast<float>(abs_x);
    float oy = static_cast<float>(abs_y + COMBO_COLLAPSED_H);
    float ow = static_cast<float>(width);
    float oh = static_cast<float>(mdv * LIST_ITEM_H);

    // Highlight follows hover; falls back to the committed selection so the
    // overlay is never blank when first opened.
    uint32_t highlight = (hover_item != UINT32_MAX) ? hover_item : selected_item;

    auto ef = neui_detail::read_widget_font(attrs.get(), 12.0f);
    neui_detail::push_widget_font(backend, ctx, ef);
    paint_scrollable_list(backend, ctx, ox, oy, ow, oh,
        items, highlight, scroll_offset,
        mdv, mdv * LIST_ITEM_H,
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

  // Layout constants for the popup menu overlay.
  static constexpr int POPUP_ITEM_H   = 22;
  static constexpr int POPUP_PAD_X    = 12;
  static constexpr int POPUP_PAD_Y    = 4;
  static constexpr int POPUP_FONT_PX  = 13;
  static constexpr int POPUP_SEP_H    = 7;
  static constexpr int POPUP_MIN_W    = 140;

  static bool popup_item_is_separator(const std::string& s)
  {
    return s.size() == 1 && s[0] == '-';
  }

  static int popup_item_height(const std::string& s)
  {
    return popup_item_is_separator(s) ? POPUP_SEP_H : POPUP_ITEM_H;
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
        float w = backend->measure_text(ctx, s.c_str(), -1, (float)POPUP_FONT_PX);
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
    if (lx < _popup_x_abs || lx >= _popup_x_abs + width ||
        ly < _popup_y_abs || ly >= _popup_y_abs + height) {
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
    if (lx < _popup_x_abs || lx >= _popup_x_abs + width ||
        ly < _popup_y_abs || ly >= _popup_y_abs + height) {
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
        if (!popup_item_is_separator(_popup_items[next - 1])) break;
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
        s.c_str(), (float)POPUP_FONT_PX,
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
    float ox = static_cast<float>(cb->abs_x);
    float oy = static_cast<float>(cb->abs_y + COMBO_COLLAPSED_H);
    float ow = static_cast<float>(cb->width);
    float oh = static_cast<float>(full_vis * LIST_ITEM_H);

    bool in_overlay = (lx >= ox && lx < ox + ow && ly >= oy && ly < oy + oh);

    if (in_overlay && n > 0) {
      // Check scrollbar column first.
      if (lx >= ox + ow - static_cast<float>(SCROLLBAR_W) &&
          n > static_cast<uint32_t>(full_vis)) {
        SbGeom   sb    = compute_sb(full_vis * LIST_ITEM_H, full_vis, n, cb->scroll_offset);
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
      uint32_t clicked = vis_scroll + static_cast<uint32_t>(rel_y / LIST_ITEM_H);
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
    float ox = static_cast<float>(cb->abs_x);
    float oy = static_cast<float>(cb->abs_y + COMBO_COLLAPSED_H);
    float ow = static_cast<float>(cb->width);
    float oh = static_cast<float>(full_vis * LIST_ITEM_H);

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
    SbGeom   sb       = compute_sb(full_vis * LIST_ITEM_H, full_vis, n,
                                   _combo_sb_drag_start_off);
    float movable = sb.track_h - sb.thumb_h;
    if (movable > 0.0f) {
      uint32_t range  = n - static_cast<uint32_t>(full_vis);
      int delta_y     = static_cast<int>(ly) - _combo_sb_drag_start_y;
      int new_off     = static_cast<int>(_combo_sb_drag_start_off)
                      + static_cast<int>(delta_y * static_cast<float>(range)
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
    float ox = static_cast<float>(cb->abs_x);
    float oy = static_cast<float>(cb->abs_y + COMBO_COLLAPSED_H);
    float ow = static_cast<float>(cb->width);
    float oh = static_cast<float>(full_vis * LIST_ITEM_H);

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
    uint32_t row = cb->scroll_offset + static_cast<uint32_t>(rel_y / LIST_ITEM_H);
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

  static constexpr int ML_LINE_H  = 18;
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
      if (s[i] == '\n') starts.push_back(i + 1);
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
      if (starts[mid] <= pos) lo = mid; else hi = mid - 1;
    }
    return lo;
  }

  // End byte offset of the given line (exclusive of the '\n'; equals text.size()
  // for the last line).
  static int ml_line_end(const std::string& s, const std::vector<int>& starts,
                          int line)
  {
    if (line + 1 < static_cast<int>(starts.size()))
      return starts[line + 1] - 1;   // position of the '\n'
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
    int n = avail / ML_LINE_H;
    return n > 0 ? n : 1;
  }

  static void ml_scroll_to_cursor(MultilineWidget& ml,
                                   const std::vector<int>& starts)
  {
    int line = ml_line_from_pos(starts, ml.cursor_pos);
    int vis  = ml_visible_lines(ml.height);
    if (line < static_cast<int>(ml.scroll_offset))
      ml.scroll_offset = line;
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

    auto   starts = ml_line_starts(text);
    uint32_t n_lines = static_cast<uint32_t>(starts.size());
    int    vis     = ml_visible_lines(height);
    bool   show_sb = n_lines > static_cast<uint32_t>(vis);
    float  content_w = show_sb ? fw - static_cast<float>(SCROLLBAR_W) : fw;

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
    int sel_lo = std::min(cursor_pos, sel_anchor);
    int sel_hi = std::max(cursor_pos, sel_anchor);
    bool has_sel = (sel_lo != sel_hi);

    int max_visible = (static_cast<int>(fh) - 2 * ML_PAD_Y + ML_LINE_H - 1) / ML_LINE_H;
    if (max_visible < 1) max_visible = 1;

    if (backend->push_clip) backend->push_clip(ctx, fx, fy, content_w, fh);

    for (int i = 0; i < max_visible; ++i) {
      int line = static_cast<int>(scroll_offset) + i;
      if (line >= static_cast<int>(n_lines)) break;
      int ls = starts[line];
      int le = ml_line_end(text, starts, line);

      float row_y   = fy + static_cast<float>(ML_PAD_Y + i * ML_LINE_H);
      float row_h   = static_cast<float>(ML_LINE_H);
      float base_x  = fx + static_cast<float>(ML_PAD_X);
      float avail_w = content_w - static_cast<float>(2 * ML_PAD_X);

      // Selection highlight for this line.
      if (has_sel && sel_lo <= le && sel_hi >= ls && backend->measure_text) {
        int lo = std::max(sel_lo, ls);
        int hi = std::min(sel_hi, le);
        float x0 = base_x + backend->measure_text(ctx, text.c_str() + ls,
                                                   lo - ls, ef.size);
        float x1 = base_x + backend->measure_text(ctx, text.c_str() + ls,
                                                   hi - ls, ef.size);
        // If the selection extends past the end-of-line newline, draw a small
        // trailing strip so the user sees the newline itself as selected.
        if (sel_hi > le) x1 += 6.0f;
        if (x1 > x0)
          backend->fill_rect(ctx, x0, row_y, x1 - x0, row_h,
                              neui_detail::color(ColorRole::accent_translucent));
      }

      // Line text.
      if (le > ls && backend->draw_text) {
        // Render just this line's slice - draw_text takes a null-terminated
        // pointer + len-via-\0. We copy to a temp to avoid modifying text.
        std::string seg = text.substr(ls, le - ls);
        backend->draw_text(ctx, base_x, row_y, avail_w, row_h,
                           seg.c_str(), ef.size,
                           neui_detail::color(ColorRole::text_primary));
      }
    }

    // Caret + IME composition overlay.
    if (is_focused && backend->measure_text) {
      int line = ml_line_from_pos(starts, cursor_pos);
      if (line >= static_cast<int>(scroll_offset) &&
          line <  static_cast<int>(scroll_offset) + vis) {
        int   ls   = starts[line];
        float col  = backend->measure_text(ctx, text.c_str() + ls,
                                            cursor_pos - ls, ef.size);
        float cx   = fx + static_cast<float>(ML_PAD_X) + col;
        float cy   = fy + static_cast<float>(ML_PAD_Y
                     + (line - static_cast<int>(scroll_offset)) * ML_LINE_H);
        if (composing && !composition_text.empty() && backend->draw_text) {
          // Draw composition string at the caret position with per-clause
          // underlines. Suppress the regular caret while composing - the
          // IME's candidate window provides the visible caret position.
          float comp_w = backend->measure_text(ctx, composition_text.c_str(),
                                               static_cast<int>(composition_text.size()),
                                               ef.size);
          backend->draw_text(ctx, cx, cy, comp_w,
                             static_cast<float>(ML_LINE_H),
                             composition_text.c_str(),
                             ef.size,
                             neui_detail::color(ColorRole::text_primary));
          paint_composition_underline(backend, ctx, cx,
                                       cy + static_cast<float>(ML_LINE_H) - 2.0f,
                                       composition_text, composition_attrs, ef.size);
        } else {
          backend->fill_rect(ctx, cx, cy, 1.5f, static_cast<float>(ML_LINE_H),
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
    int ls   = starts[line];
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
      return starts[line];
    neui_render_ctx_t ctx = nullptr;
    for (uint32_t p : ml.session->_widgets.get_all_parents(ml.index)) {
      if (p == 0) continue;
      if (ml.session->_widgets.exists(p)) {
        ctx = ml.session->_widgets[p].render_ctx;
        if (ctx) break;
      }
    }
    if (!ctx) return starts[line];

    auto ef = neui_detail::read_widget_font(ml.attrs.get(), ML_FONT_SIZE);
    neui_detail::push_widget_font(ml.session->_backend, ctx, ef);
    int ls  = starts[line];
    int le  = ml_line_end(ml.text, starts, line);
    int pos = ls;
    float prev_w = 0.0f;
    int result = le;
    while (pos < le) {
      int char_end = pos + utf8_char_len(ml.text, pos);
      float end_w  = ml.session->_backend->measure_text(
        ctx, ml.text.c_str() + ls, char_end - ls, ef.size);
      if (col_px < (prev_w + end_w) * 0.5f) { result = pos; break; }
      prev_w = end_w;
      pos    = char_end;
    }
    neui_detail::pop_widget_font(ml.session->_backend, ctx, ef);
    return result;
  }

  bool MultilineWidget::on_keydown(uint32_t keycode, uint32_t modifiers)
  {
    bool shift    = (modifiers & 1) != 0;
    bool ctrl     = (modifiers & 2) != 0;
    bool readonly = ml_readonly(*this);

    auto starts = ml_line_starts(text);
    int  text_len = static_cast<int>(text.size());

    auto has_sel  = [&]{ return sel_anchor != cursor_pos; };
    auto sel_lo   = [&]{ return std::min(cursor_pos, sel_anchor); };
    auto sel_hi   = [&]{ return std::max(cursor_pos, sel_anchor); };
    auto collapse = [&]{ sel_anchor = cursor_pos; };
    auto del_sel  = [&]{
      int lo = sel_lo(), hi = sel_hi();
      text.erase(lo, hi - lo);
      cursor_pos = sel_anchor = lo;
    };
    auto snapshot = [&]() -> neui_detail::EditState {
      return { text, cursor_pos, sel_anchor };
    };

    switch (keycode) {
    case NEUI_KEY_LEFT:
      history.reset_action();
      if (!shift && !ctrl && has_sel()) { cursor_pos = sel_lo(); collapse(); }
      else if (ctrl) {
        cursor_pos = word_left(text, cursor_pos);
        if (!shift) collapse();
      } else {
        if (cursor_pos > 0) cursor_pos = utf8_prev_start(text, cursor_pos);
        if (!shift) collapse();
      }
      ml_scroll_to_cursor(*this, starts);
      repaint(); return true;

    case NEUI_KEY_RIGHT:
      history.reset_action();
      if (!shift && !ctrl && has_sel()) { cursor_pos = sel_hi(); collapse(); }
      else if (ctrl) {
        cursor_pos = word_right(text, cursor_pos);
        if (!shift) collapse();
      } else {
        if (cursor_pos < text_len) cursor_pos += utf8_char_len(text, cursor_pos);
        if (!shift) collapse();
      }
      ml_scroll_to_cursor(*this, starts);
      repaint(); return true;

    case NEUI_KEY_UP: {
      history.reset_action();
      int line = ml_line_from_pos(starts, cursor_pos);
      if (line > 0) {
        float col = ml_col_px(*this, starts, cursor_pos);
        cursor_pos = ml_pos_from_col(*this, starts, line - 1, col);
      } else {
        cursor_pos = 0;
      }
      if (!shift) collapse();
      ml_scroll_to_cursor(*this, starts);
      repaint(); return true;
    }

    case NEUI_KEY_DOWN: {
      history.reset_action();
      int line = ml_line_from_pos(starts, cursor_pos);
      if (line + 1 < static_cast<int>(starts.size())) {
        float col = ml_col_px(*this, starts, cursor_pos);
        cursor_pos = ml_pos_from_col(*this, starts, line + 1, col);
      } else {
        cursor_pos = text_len;
      }
      if (!shift) collapse();
      ml_scroll_to_cursor(*this, starts);
      repaint(); return true;
    }

    case NEUI_KEY_HOME: {
      history.reset_action();
      if (ctrl) cursor_pos = 0;
      else {
        int line = ml_line_from_pos(starts, cursor_pos);
        cursor_pos = starts[line];
      }
      if (!shift) collapse();
      ml_scroll_to_cursor(*this, starts);
      repaint(); return true;
    }

    case NEUI_KEY_END: {
      history.reset_action();
      if (ctrl) cursor_pos = text_len;
      else {
        int line = ml_line_from_pos(starts, cursor_pos);
        cursor_pos = ml_line_end(text, starts, line);
      }
      if (!shift) collapse();
      ml_scroll_to_cursor(*this, starts);
      repaint(); return true;
    }

    case NEUI_KEY_BACK:
      if (readonly) return true;
      if (has_sel() || cursor_pos > 0) {
        history.mark(snapshot(), neui_detail::EditHistory::Deleting, has_sel());
        if (has_sel()) del_sel();
        else {
          int start = ctrl ? word_left(text, cursor_pos)
                           : utf8_prev_start(text, cursor_pos);
          text.erase(start, cursor_pos - start);
          cursor_pos = sel_anchor = start;
        }
      }
      ml_scroll_to_cursor(*this, ml_line_starts(text));
      repaint(); return true;

    case NEUI_KEY_DELETE:
      if (readonly) return true;
      if (has_sel() || cursor_pos < text_len) {
        history.mark(snapshot(), neui_detail::EditHistory::Deleting, has_sel());
        if (has_sel()) del_sel();
        else {
          int end = ctrl ? word_right(text, cursor_pos)
                         : cursor_pos + utf8_char_len(text, cursor_pos);
          text.erase(cursor_pos, end - cursor_pos);
        }
      }
      ml_scroll_to_cursor(*this, ml_line_starts(text));
      repaint(); return true;

    case NEUI_KEY_RETURN:
      if (readonly) return true;
      // Newline is its own undo group - never coalesces with surrounding typing.
      history.mark(snapshot(), neui_detail::EditHistory::None, has_sel());
      if (has_sel()) del_sel();
      text.insert(cursor_pos, 1, '\n');
      cursor_pos += 1;
      sel_anchor  = cursor_pos;
      ml_scroll_to_cursor(*this, ml_line_starts(text));
      repaint(); return true;

    case NEUI_KEY_A:
      if (ctrl) {
        history.reset_action();
        sel_anchor = 0;
        cursor_pos = text_len;
        repaint();
        return true;
      }
      break;

    case NEUI_KEY_C:
      if (ctrl) {
        if (has_sel()) {
          int lo = sel_lo(), hi = sel_hi();
          platform_clipboard_set_text(
            text.c_str() + lo, static_cast<uint32_t>(hi - lo));
        }
        return true;
      }
      break;

    case NEUI_KEY_X:
      if (ctrl) {
        if (has_sel()) {
          int lo = sel_lo(), hi = sel_hi();
          platform_clipboard_set_text(
            text.c_str() + lo, static_cast<uint32_t>(hi - lo));
          if (!readonly) {
            history.mark(snapshot(), neui_detail::EditHistory::None, true);
            del_sel();
            ml_scroll_to_cursor(*this, ml_line_starts(text));
            repaint();
          }
        }
        return true;
      }
      break;

    case NEUI_KEY_V:
      if (ctrl) {
        if (readonly) return true;
        int n = platform_clipboard_get_text(nullptr, 0);
        if (n <= 0) return true;
        std::vector<char> buf(static_cast<size_t>(n));
        platform_clipboard_get_text(buf.data(), n);
        // Drop trailing null; normalise CRLF to LF (multiline keeps newlines).
        std::string paste;
        paste.reserve(buf.size());
        for (size_t i = 0; i + 1 < buf.size(); ++i) {
          char c = buf[i];
          if (c == '\r') continue;
          paste.push_back(c);
        }
        history.mark(snapshot(), neui_detail::EditHistory::None, has_sel());
        if (has_sel()) del_sel();
        text.insert(cursor_pos, paste);
        cursor_pos += static_cast<int>(paste.size());
        sel_anchor = cursor_pos;
        ml_scroll_to_cursor(*this, ml_line_starts(text));
        repaint();
        return true;
      }
      break;

    case NEUI_KEY_Z:
      if (ctrl) {
        neui_detail::EditState restored;
        bool ok = shift ? history.redo(snapshot(), restored)
                        : history.undo(snapshot(), restored);
        if (ok) {
          text       = restored.text;
          cursor_pos = restored.cursor;
          sel_anchor = restored.anchor;
          ml_scroll_to_cursor(*this, ml_line_starts(text));
          repaint();
        }
        return true;
      }
      break;

    case NEUI_KEY_Y:
      if (ctrl) {
        neui_detail::EditState restored;
        if (history.redo(snapshot(), restored)) {
          text       = restored.text;
          cursor_pos = restored.cursor;
          sel_anchor = restored.anchor;
          ml_scroll_to_cursor(*this, ml_line_starts(text));
          repaint();
        }
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

  // Encode a codepoint as UTF-8 into `out` (up to 4 bytes). Returns byte count.
  static int ml_encode_utf8(uint32_t cp, char out[4])
  {
    if (cp < 0x80) { out[0] = static_cast<char>(cp); return 1; }
    if (cp < 0x800) {
      out[0] = static_cast<char>(0xC0 | (cp >> 6));
      out[1] = static_cast<char>(0x80 | (cp & 0x3F));
      return 2;
    }
    if (cp < 0x10000) {
      out[0] = static_cast<char>(0xE0 | (cp >> 12));
      out[1] = static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
      out[2] = static_cast<char>(0x80 | (cp & 0x3F));
      return 3;
    }
    out[0] = static_cast<char>(0xF0 | (cp >> 18));
    out[1] = static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
    out[2] = static_cast<char>(0x80 | ((cp >>  6) & 0x3F));
    out[3] = static_cast<char>(0x80 | (cp & 0x3F));
    return 4;
  }

  bool MultilineWidget::on_keychar(uint32_t codepoint, uint32_t /*modifiers*/)
  {
    if (ml_readonly(*this)) return false;
    if (codepoint < 0x20 || codepoint == 0x7F) return false;

    char buf[4];
    int  n = ml_encode_utf8(codepoint, buf);

    bool has_selection = (sel_anchor != cursor_pos);
    history.mark(neui_detail::EditState{ text, cursor_pos, sel_anchor },
                 neui_detail::EditHistory::Typing, has_selection);
    if (has_selection) {
      int lo = std::min(cursor_pos, sel_anchor);
      int hi = std::max(cursor_pos, sel_anchor);
      text.erase(lo, hi - lo);
      cursor_pos = sel_anchor = lo;
    }
    text.insert(cursor_pos, buf, n);
    cursor_pos += n;
    sel_anchor  = cursor_pos;

    auto starts = ml_line_starts(text);
    ml_scroll_to_cursor(*this, starts);
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
    auto starts = ml_line_starts(text);
    int  line   = ml_line_from_pos(starts, cursor_pos);
    int  vis    = ml_visible_lines(height);
    if (line < static_cast<int>(scroll_offset) ||
        line >= static_cast<int>(scroll_offset) + vis)
      return false;
    int   ls  = starts[line];
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
                          + (line - static_cast<int>(scroll_offset)) * ML_LINE_H);
    if (out_h) *out_h = static_cast<float>(ML_LINE_H);
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
      composition_text.assign(utf8 ? utf8 : "", byte_len > 0 ? byte_len : 0);
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
      bool has_sel = (sel_anchor != cursor_pos);
      history.mark(composition_pre_state,
                   neui_detail::EditHistory::Typing, has_sel);
      if (has_sel) {
        int lo = std::min(cursor_pos, sel_anchor);
        int hi = std::max(cursor_pos, sel_anchor);
        text.erase(lo, hi - lo);
        cursor_pos = sel_anchor = lo;
      }
      if (utf8 && byte_len > 0) {
        text.insert(cursor_pos, utf8, byte_len);
        cursor_pos += byte_len;
        sel_anchor  = cursor_pos;
      }
      composition_pre_state = neui_detail::EditState{ text, cursor_pos, sel_anchor };
      composition_text.clear();
      composition_caret = 0;
      composition_attrs.clear();

      auto starts = ml_line_starts(text);
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

  bool MultilineWidget::on_mouse_event(neui_event_t* event)
  {
    auto starts = ml_line_starts(text);
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
                      + static_cast<int>(delta_y * static_cast<float>(range)
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
    int row   = rel_y / ML_LINE_H;
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

  static constexpr int TREE_ROW_H      = 20;
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

    int full_vis = std::max(1, height / TREE_ROW_H);
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
    int max_visible = (height + TREE_ROW_H - 1) / TREE_ROW_H;
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

      float row_y = fy + static_cast<float>(i * TREE_ROW_H);
      float rowh  = static_cast<float>(TREE_ROW_H);
      float indent_px = static_cast<float>(TREE_LEFT_PAD + vr.depth * TREE_INDENT);

      bool sel = (vr.id == selected_tree_item);
      // Selection background.
      if (sel)
        backend->fill_rect(ctx, fx + 1.0f, row_y, content_w - 2.0f, rowh,
                           neui_detail::color(is_focused
                              ? ColorRole::accent
                              : ColorRole::control_bg_inactive));

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
      if (tv_item_enabled(tv, rows[i].id)) return i;
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

    int full_vis = std::max(1, height / TREE_ROW_H);
    int cur      = find_row(rows, selected_tree_item);
    uint32_t prev_sel = selected_tree_item;

    switch (keycode) {
    case NEUI_KEY_UP: {
      // Find the next enabled row above the current selection.
      int target = (cur < 0) ? tv_next_enabled_row(*this, rows, -1, +1)
                              : tv_next_enabled_row(*this, rows, cur, -1);
      if (target >= 0) selected_tree_item = rows[target].id;
      break;
    }
    case NEUI_KEY_DOWN: {
      int target = (cur < 0) ? tv_next_enabled_row(*this, rows, -1, +1)
                              : tv_next_enabled_row(*this, rows, cur, +1);
      if (target >= 0) selected_tree_item = rows[target].id;
      break;
    }
    case NEUI_KEY_HOME: {
      int target = tv_next_enabled_row(*this, rows, -1, +1);
      if (target >= 0) selected_tree_item = rows[target].id;
      break;
    }
    case NEUI_KEY_END: {
      int target = tv_next_enabled_row(*this, rows,
                                        static_cast<int>(rows.size()), -1);
      if (target >= 0) selected_tree_item = rows[target].id;
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
      if (rows[cur].has_children) {
        if (!it->second.expanded) {
          it->second.expanded = true;
        } else {
          // Already expanded -> move to first enabled child.
          if (cur + 1 < static_cast<int>(rows.size()) &&
              rows[cur + 1].depth > rows[cur].depth) {
            int target = tv_item_enabled(*this, rows[cur + 1].id)
                           ? cur + 1
                           : tv_next_enabled_row(*this, rows, cur + 1, +1);
            // Only accept the target if it's still under our subtree.
            if (target >= 0 && target < static_cast<int>(rows.size()) &&
                rows[target].depth > rows[cur].depth)
              selected_tree_item = rows[target].id;
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
      if (it != tree_items.end() && rows[cur].has_children)
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
    int full_vis = std::max(1, height / TREE_ROW_H);
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
                      + static_cast<int>(delta_y * static_cast<float>(range)
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
      uint32_t row = scroll_offset + static_cast<uint32_t>(rel_y / TREE_ROW_H);
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
    uint32_t row = scroll_offset + static_cast<uint32_t>(rel_y / TREE_ROW_H);
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

  static GridViewport xpl_grid_viewport(const GridWidget& g)
  {
    auto cfg = xpl_grid_config(g);
    return neui_detail::grid_compute_viewport(g.model, g.width, g.height,
                                                cfg.row_h, cfg.header_h);
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
    if (n_rows == 0) return false;

    int prev_row = model.selected_row;
    int prev_col = model.selected_col;
    int vis = grid_visible_rows(vp, cfg.row_h);
    if (vis < 1) vis = 1;
    bool handled = true;

    switch (keycode) {
    case NEUI_KEY_UP:
      if (model.selected_row > 0)        model.selected_row--;
      else if (model.selected_row < 0)   model.selected_row = 0;
      break;
    case NEUI_KEY_DOWN:
      if (model.selected_row < n_rows - 1) {
        if (model.selected_row < 0) model.selected_row = 0;
        else                         model.selected_row++;
      }
      break;
    case NEUI_KEY_PAGEUP:
      model.selected_row = (model.selected_row < 0)
        ? 0
        : std::max(0, model.selected_row - vis);
      break;
    case NEUI_KEY_PAGEDOWN:
      model.selected_row = (model.selected_row < 0)
        ? std::min(n_rows - 1, vis)
        : std::min(n_rows - 1, model.selected_row + vis);
      break;
    case NEUI_KEY_HOME:
      if (cfg.cell_focus && !(modifiers & NEUI_KMOD_CTRL)) {
        model.selected_col = (n_cols > 0) ? 0 : -1;
        if (model.selected_row < 0) model.selected_row = 0;
      } else {
        model.selected_row = 0;
        if (cfg.cell_focus) model.selected_col = (n_cols > 0) ? 0 : -1;
      }
      break;
    case NEUI_KEY_END:
      if (cfg.cell_focus && !(modifiers & NEUI_KMOD_CTRL)) {
        model.selected_col = (n_cols > 0) ? n_cols - 1 : -1;
        if (model.selected_row < 0) model.selected_row = n_rows - 1;
      } else {
        model.selected_row = n_rows - 1;
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

  bool GridWidget::on_mouse_event(neui_event_t* event)
  {
    using namespace neui_detail;
    auto cfg = grid_read_config(attrs.get());
    GridViewport vp = grid_compute_viewport(model, width, height,
                                              cfg.row_h, cfg.header_h);

    int lx = event->data.mouse.x - abs_x;
    int ly = event->data.mouse.y - abs_y;

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
        platform_set_cursor(NEUI_CURSOR_EW_RESIZE);
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
        platform_set_cursor(NEUI_CURSOR_DEFAULT);
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
      platform_set_cursor(hit.region == GridHitRegion::HeaderDivider
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
      GridHit hit = grid_hit_test(model, vp, cfg.row_h,
                                    width, height, lx, ly);
      switch (hit.region) {
      case GridHitRegion::HeaderDivider:
        if (event->type == NEUI_EVENT_MOUSE_BUTTON_DOWN) {
          model.column_resize_col     = hit.col;
          model.column_resize_start_x = event->data.mouse.x;
          model.column_resize_start_w = model.columns[(size_t)hit.col].width;
          model.column_resize_old_w   = model.column_resize_start_w;
          platform_set_cursor(NEUI_CURSOR_EW_RESIZE);
          return true;
        }
        return true;
      case GridHitRegion::Header:
        // Reserved for column-header click semantics (sort etc).
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
          // Selection already updated by the prior DOWN; just activate.
          xpl_grid_fire_row_activated(*this, hit.row);
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
