#include <cstring>
#include <algorithm>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "host.h"
#include "platform.h"
#include "../shared/shortcut_format.h"
#ifdef _WIN32
// Win32-specific helpers used directly from the host's API entry points
// (icon application, accel-table build/translate). The macOS port replaces
// these with platform_* shims (see plans/macos-port.md).
#include "../shared/win32/icon_win32.h"
#include "../shared/win32/accel_table_win32.h"
#endif

namespace xpl_host
{
  extern std::vector<std::unique_ptr<Session>> sessions;

  // Widget id layout: upper 16 = owning session id, lower 16 = tree slot.
  // The lower-16 slot index is what the Tree<> uses; the upper 16 lets the
  // boundary validate that a handle from session A isn't being applied to
  // session B (relevant for audio plugins where many sessions live in one
  // process). Internal Session methods assume the widget belongs to them
  // and only mask the lower 16; the boundary helpers below enforce that.

  static uint32_t WidgetToIndex(neui_widget_t widget)
  {
    return widget.id & 0xffff;
  }

  static neui_widget_t IndexToWidget(uint32_t session_id, uint32_t idx)
  {
    return { ((session_id & 0xffff) << 16) | (idx & 0xffff) };
  }

  static Session* get_session(neui_session_t session)
  {
    uint32_t idx = (session.session & 0xffff) - 1;
    if (idx < sessions.size())
      return sessions[idx].get();
    return nullptr;
  }

  // True if `widget` is either a sentinel (widget_root / widget_none) or a
  // properly-packed handle whose session id matches `session_id`. The
  // sentinels pass through because clients legitimately use widget_root as
  // the parent argument and widget_none as the "no widget" return value.
  static bool widget_belongs_to_session(neui_widget_t widget, uint32_t session_id)
  {
    if (widget.id == 0)          return true;   // widget_root
    if (widget.id == UINT32_MAX) return true;   // widget_none
    return ((widget.id >> 16) & 0xffff) == (session_id & 0xffff);
  }

  // Resolve the session for an API call that operates on a specific widget.
  // Returns nullptr if the session is invalid OR the widget belongs to a
  // different session - the call is silently dropped (as for an invalid
  // session today).
  static Session* get_session_for_widget(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (!widget_belongs_to_session(widget, s->_session_id)) return nullptr;
    return s;
  }

  // -------------------------------------------------------------------------
  // Recursive helpers

  // Recursively fire ondestroy for a widget and all its descendants (depth-first).
  static void destroy_recursive(Session* s, uint32_t idx,
                                 neui_widget_client_t* client_api, void* token)
  {
    {
      auto* wd = s->get_widget(idx);
      if (!wd) return;
      uint32_t child = s->_widgets.child(idx);
      while (child != 0) {
        uint32_t next = s->_widgets.next(child);
        destroy_recursive(s, child, client_api, token);
        child = next;
      }
    }

    auto* wd = s->get_widget(idx);
    if (!wd) return;

    // Release render context and native window.
    if (wd->render_ctx && s->_backend) {
      s->_asset_manager.release_context(wd->render_ctx, s->_backend);
      s->_backend->destroy_context(wd->render_ctx);
      wd->render_ctx = nullptr;
    }
    if (wd->native_handle) {
      // Revoke the IDropTarget / NSDraggingDestination before destroying
      // the OS handle. No-op for non-frame widgets and on platforms
      // without DnD.
      platform_dnd_unregister_window(wd->native_handle);
      platform_destroy_window(*wd);
      wd->native_handle = nullptr;
    }

    // Let the widget class clean up its own resources (e.g. MenubarWidget tears
    // down HMENU handles and removes itself from _menubars).
    wd->on_destroy(s);

    if (client_api && client_api->ondestroy)
      client_api->ondestroy(token, IndexToWidget(s->_session_id, idx), wd->userdata);

    s->_widgets.remove(idx);
  }

  // -------------------------------------------------------------------------
  // Widget factory - creates the right derived type for each widget type string.

  static std::unique_ptr<WidgetData> make_widget(const char* type)
  {
    if (!strcmp(type, NEUI_W_APPWINDOW) ||
        !strcmp(type, NEUI_W_PLUGWINDOW) ||
        !strcmp(type, NEUI_W_DIALOG))
      return std::make_unique<FrameWidget>();
    if (!strcmp(type, NEUI_W_LABEL))
      return std::make_unique<LabelWidget>();
    if (!strcmp(type, NEUI_W_SECTION))
      return std::make_unique<SectionWidget>();
    if (!strcmp(type, NEUI_W_BUTTON))
      return std::make_unique<ButtonWidget>();
    if (!strcmp(type, NEUI_W_INPUTBOX))
      return std::make_unique<InputBoxWidget>();
    if (!strcmp(type, NEUI_W_CHECKBOX) || !strcmp(type, NEUI_W_CHECKBOX3))
      return std::make_unique<CheckboxWidget>();
    if (!strcmp(type, NEUI_W_LISTBOX))
      return std::make_unique<ListItemsWidget>();
    if (!strcmp(type, NEUI_W_COMBOBOX))
      return std::make_unique<ComboBoxWidget>();
    if (!strcmp(type, NEUI_W_MULTILINE))
      return std::make_unique<MultilineWidget>();
    if (!strcmp(type, NEUI_W_TREEVIEW))
      return std::make_unique<TreeviewWidget>();
    if (!strcmp(type, NEUI_W_MENUBAR))
      return std::make_unique<MenubarWidget>();
    if (!strcmp(type, NEUI_W_IMAGE))
      return std::make_unique<ImageWidget>();
    if (!strcmp(type, NEUI_W_SLIDER))
      return std::make_unique<SliderWidget>();
    if (!strcmp(type, NEUI_W_KNOB))
      return std::make_unique<KnobWidget>();
    if (!strcmp(type, NEUI_W_CUSTOMDRAW))
      return std::make_unique<CustomDrawWidget>();
    if (!strcmp(type, NEUI_W_GRID))
      return std::make_unique<GridWidget>();
    if (!strcmp(type, NEUI_W_TABVIEW))
      return std::make_unique<TabViewWidget>();
    if (!strcmp(type, NEUI_W_TABPAGE))
      return std::make_unique<TabPageWidget>();
    return std::make_unique<WidgetData>(); // fallback for unknown types
  }

  // -------------------------------------------------------------------------
  // Widget API

  static neui_widget_t NEUI_ABI w_create(neui_session_t session,
                                          neui_widget_t parent,
                                          const char* type,
                                          int x, int y, int width, int height,
                                          void* userdata)
  {
    auto* s = get_session_for_widget(session, parent);
    if (!s || !type) return { UINT32_MAX };

    uint32_t parent_idx = (parent.id == UINT32_MAX) ? 0 : WidgetToIndex(parent);

    auto obj = make_widget(type);
    obj->type      = type;
    obj->x         = x;
    obj->y         = y;
    obj->width     = width;
    obj->height    = height;
    obj->userdata  = userdata;
    // Default visible=true so widgets created post-show appear without
    // requiring an explicit show() call. Clients that want a hidden widget
    // call widgets->hide after create; that intent is honoured on the very
    // first paint (frame widget_show no longer re-shows descendants, so a
    // pre-show hide() is not clobbered).
    obj->visible   = true;
    obj->session   = s;
    obj->session_id = session.session;
    obj->isroot    = obj->is_frame() || obj->is_menubar();

    obj->tab_stop  = !strcmp(type, NEUI_W_BUTTON)    ||
                     !strcmp(type, NEUI_W_INPUTBOX)   ||
                     !strcmp(type, NEUI_W_CHECKBOX)   ||
                     !strcmp(type, NEUI_W_CHECKBOX3)  ||
                     !strcmp(type, NEUI_W_LISTBOX)    ||
                     !strcmp(type, NEUI_W_COMBOBOX)   ||
                     !strcmp(type, NEUI_W_MULTILINE)  ||
                     !strcmp(type, NEUI_W_TREEVIEW)   ||
                     !strcmp(type, NEUI_W_SLIDER)     ||
                     !strcmp(type, NEUI_W_KNOB)       ||
                     !strcmp(type, NEUI_W_CUSTOMDRAW) ||
                     !strcmp(type, NEUI_W_GRID)        ||
                     !strcmp(type, NEUI_W_TABVIEW);

    obj->emit_events = !strcmp(type, NEUI_W_BUTTON)    ||
                       !strcmp(type, NEUI_W_INPUTBOX)   ||
                       !strcmp(type, NEUI_W_CHECKBOX)   ||
                       !strcmp(type, NEUI_W_CHECKBOX3)  ||
                       !strcmp(type, NEUI_W_LISTBOX)    ||
                       !strcmp(type, NEUI_W_COMBOBOX)   ||
                       !strcmp(type, NEUI_W_MULTILINE)  ||
                       !strcmp(type, NEUI_W_TREEVIEW)   ||
                       !strcmp(type, NEUI_W_SLIDER)     ||
                       !strcmp(type, NEUI_W_KNOB)       ||
                       !strcmp(type, NEUI_W_CUSTOMDRAW) ||
                       !strcmp(type, NEUI_W_GRID)        ||
                       !strcmp(type, NEUI_W_TABVIEW);

    uint32_t idx = s->_widgets.add_child(parent_idx, std::move(obj));
    if (idx == 0) return { UINT32_MAX };

    s->_widgets[idx].index     = idx;
    s->_widgets[idx].widget_id = IndexToWidget(s->_session_id, idx).id;

    // Map implicit type variants onto platform-neutral attributes so internal
    // behavior can be driven uniformly.
    if (!strcmp(type, NEUI_W_CHECKBOX3))
      neui_detail::ensure_attrs(s->_widgets[idx].attrs)
        .set_int(NEUI_ATTR_TRISTATE, 1);
    if (!strcmp(type, NEUI_W_MULTILINE))
      neui_detail::ensure_attrs(s->_widgets[idx].attrs)
        .set_int(NEUI_ATTR_MULTILINE, 1);

    // MENUBAR: create the native menu handle immediately.
    if (s->_widgets[idx].is_menubar()) {
      auto& mb = dynamic_cast<MenubarWidget&>(s->_widgets[idx]);
      mb.hmenu = platform_menubar_create(IndexToWidget(s->_session_id, idx).id);
      s->_menubars.push_back(idx);
    }

    return IndexToWidget(s->_session_id, idx);
  }

  static void NEUI_ABI w_destroy(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;

    // A destroyed TABPAGE drops a tab: capture the parent TABVIEW so we can
    // re-flow its strip + page geometry after the slot is freed (the selected
    // index may now be out of range). Mirrors the win32 host's widget_destroy.
    uint32_t tabview_parent = 0;
    {
      auto& w = s->_widgets[idx];
      if (w.type && !strcmp(w.type, NEUI_W_TABPAGE)) {
        uint32_t pidx = s->_widgets.get_parent(idx);
        if (pidx && s->_widgets.exists(pidx)) {
          auto& pw = s->_widgets[pidx];
          if (pw.type && !strcmp(pw.type, NEUI_W_TABVIEW)) tabview_parent = pidx;
        }
      }
    }

    neui_widget_client_t* client_api = nullptr;
    void* token = s->get_token();
    {
      auto* wd = s->get_widget(idx);
      if (wd && wd->session) {
        neui_client_t* client = s->_widgets[idx].session->_client;
        if (client)
          client_api = static_cast<neui_widget_client_t*>(
            client->get_interface(token, NEUI_API_WIDGETS));
      }
    }
    destroy_recursive(s, idx, client_api, token);

    // Re-clamp the selection + re-apply page visibility/geometry, then repaint
    // the strip so the now-correct tab count is reflected immediately (matching
    // the win32 / macOS hosts, which re-flow on every TABPAGE removal).
    if (tabview_parent && s->_widgets.exists(tabview_parent)) {
      if (auto* tv = dynamic_cast<TabViewWidget*>(&s->_widgets[tabview_parent])) {
        tv->apply_page_geometry();
        if (void* frame = s->find_parent_native_handle(tabview_parent))
          platform_invalidate(frame);
      }
    }
  }

  static void NEUI_ABI w_show(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];

    if (wd.is_frame()) {
      if (!wd.native_handle) {
        if (wd.is_dialog()) {
          // Resolve owner HWND (if any) before creating the dialog HWND so
          // CreateWindowEx can wire the parent slot correctly.
          void* owner_native = nullptr;
          if (wd.owner_index != 0 && s->_widgets.exists(wd.owner_index))
            owner_native = s->_widgets[wd.owner_index].native_handle;
          platform_create_dialog(s, idx, wd, owner_native);
        } else if (!strcmp(wd.type, NEUI_W_APPWINDOW)) {
          platform_create_appwindow(s, idx, wd);
        } else {
          platform_create_plugwindow(s, idx, wd);
        }
      }
      if (wd.native_handle) {
        // Attach any MENUBAR children to the frame window.
        uint32_t child = s->_widgets.child(idx);
        while (child != 0) {
          if (s->_widgets.exists(child)) {
            auto& cwd = s->_widgets[child];
            if (cwd.is_menubar()) {
              auto& mb = dynamic_cast<MenubarWidget&>(cwd);
              if (mb.hmenu)
                platform_menubar_attach(wd.native_handle, mb.hmenu);
            }
          }
          child = s->_widgets.next(child);
        }
        platform_show_window(wd.native_handle);

        // Register the frame as a drag&drop drop target so the OS knows
        // to route drags into our IDropTarget. The widget's drop_target
        // flag still gates whether any event reaches the client; this
        // step only opens the OS-side path. No-op on platforms without
        // DnD support.
        platform_dnd_register_window(wd.native_handle, s, wd.widget_id);

        // Block input on the owner while the dialog is up - unless the
        // client opted into modeless via NEUI_ATTR_MODAL = 0.
        bool is_modal_dialog = false;
        if (wd.is_dialog() && wd.owner_index != 0 &&
            s->_widgets.exists(wd.owner_index)) {
          is_modal_dialog = !wd.attrs ||
                            wd.attrs->get_int(NEUI_ATTR_MODAL, 1) != 0;
          void* owner_native = s->_widgets[wd.owner_index].native_handle;
          if (owner_native && is_modal_dialog)
            platform_set_window_enabled(owner_native, false);
        }
        // Native blocking modal: spin a nested OS pump until the dialog
        // is destroyed. The destroy path (platform_win32.cpp WM_NCDESTROY /
        // platform_macos.mm windowWillClose) clears modal_pump_active so
        // the pump exits and widget_show returns to the caller.
        if (is_modal_dialog) {
          auto* fw = dynamic_cast<FrameWidget*>(&wd);
          if (fw) {
            fw->modal_pump_active = true;
            platform_run_modal_until(&fw->modal_pump_active);
          }
        }
      }
    } else {
      wd.visible = true;
    }
  }

  static void NEUI_ABI w_hide(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];

    if (wd.is_frame() && wd.native_handle)
      platform_hide_window(wd.native_handle);
    else
      wd.visible = false;
  }

  static void NEUI_ABI w_set_pos(neui_session_t session, neui_widget_t widget,
                                  int x, int y, int width, int height)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];

    wd.x = x; wd.y = y; wd.width = width; wd.height = height;

    if (wd.is_frame() && wd.native_handle)
      platform_set_window_pos(wd.native_handle, x, y, width, height, wd.dpi);
  }

  static void NEUI_ABI w_set_size(neui_session_t session, neui_widget_t widget,
                                   int width, int height)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];

    wd.width = width; wd.height = height;

    if (wd.is_frame() && wd.native_handle)
      platform_set_window_pos(wd.native_handle, wd.x, wd.y, width, height, wd.dpi);
  }

  static void NEUI_ABI w_set_emit_events(neui_session_t session,
                                          neui_widget_t widget, bool enabled)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (s->_widgets.exists(idx))
      s->_widgets[idx].emit_events = enabled;
  }

  static void NEUI_ABI w_set_text(neui_session_t session,
                                   neui_widget_t widget, const char* text)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];
    wd.text = text ? text : "";

    // MULTILINE caches line-start offsets for paint; an external text change
    // invalidates that cache.
    if (auto* ml = dynamic_cast<MultilineWidget*>(&wd))
      ml->mark_lines_dirty();

    // Frame windows: push the text into the native title bar when live.
    if (wd.is_frame() && wd.native_handle)
      platform_set_window_title(wd.native_handle, wd.text.c_str());

    // IMAGE widget: set_text is the path source. Drop any bound asset
    // handle so the path becomes the live source (mutual-clear with
    // set_asset).
    if (auto* img = dynamic_cast<ImageWidget*>(&wd))
      img->asset = asset_none;

    // A text/path change is a visible change -> repaint the owning frame, the
    // same way w_set_asset and the attribute setters do. Without this, the
    // self-painted xpl widgets (LABEL / BUTTON / MULTILINE / IMAGE / ...) keep
    // showing the old text until some unrelated event forces a paint. For a
    // top-level frame this returns nullptr (no parent HWND) and no-ops, which
    // is correct - the title bar was already updated above.
    if (void* frame = s->find_parent_native_handle(idx))
      platform_invalidate(frame);
  }

  // Forward decls: COMPONENT attach helper + the one-call instantiate thunk.
  // Defined alongside the component asset thunks (they need compound_api /
  // behavior_api, declared later in this TU).
  static void attach_component(Session* s, uint32_t idx,
                               neui_detail::AssetEntry* ce);
  static neui_widget_t NEUI_ABI w_create_from_component(
      neui_session_t session, neui_widget_t parent, neui_asset_t component,
      int x, int y, int width, int height);

  // Bind an asset handle as the IMAGE widget's source. Drops any path
  // source (wd.text) so the asset becomes sole source - mirrors the
  // win32 native host's contract and the painter's draw_asset thunk
  // for slot resolution + cross-session rejection. asset_none clears.
  static void NEUI_ABI w_set_asset(neui_session_t session,
                                    neui_widget_t widget, neui_asset_t asset)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];

    if (asset.id != asset_none.id &&
        ((asset.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) {
      return;  // cross-session - silent reject
    }

    if (auto* img = dynamic_cast<ImageWidget*>(&wd)) {
      img->asset = asset;
      img->text.clear();
    } else if (auto* cd = dynamic_cast<CustomDrawWidget*>(&wd)) {
      // Kind-route: COMPOUND -> visual slot, BEHAVIOR -> input slot.
      // asset_none clears whichever slot the kind would land in - but
      // since asset_none has no kind, we use it to clear the compound
      // slot (matching the v1 contract for IMAGE/CUSTOMDRAW).
      if (asset.id == asset_none.id) {
        cd->compound_asset = asset_none;
      } else {
        uint32_t slot = asset.id & 0xffff;
        auto* entry  = s->_asset_manager.get_slot(slot);
        if (entry && entry->kind == NEUI_ASSET_KIND_BEHAVIOR) {
          cd->behavior_asset = asset;
        } else if (entry && entry->kind == NEUI_ASSET_KIND_COMPONENT) {
          // A COMPONENT bundles a compound + behavior + defaults: attach both
          // slots and stamp the defaults (so a component can be applied to an
          // already-created CUSTOMDRAW, not only via create_from_component).
          attach_component(s, idx, entry);
        } else {
          // Default route (BITMAP / COMPOUND / null entry): compound slot.
          cd->compound_asset = asset;
        }
      }
    } else {
      return;  // non-IMAGE/CUSTOMDRAW: no-op
    }

    // Trigger a repaint via the owning frame.
    if (void* frame = s->find_parent_native_handle(idx))
      platform_invalidate(frame);
  }

  static int NEUI_ABI w_get_text(neui_session_t session,
                                  neui_widget_t widget,
                                  char* buf, int buflen)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return -1;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return -1;

    const auto& text = s->_widgets[idx].text;
    int need = static_cast<int>(text.size()) + 1;
    if (buf && buflen > 0) {
      int copy = std::min(buflen - 1, static_cast<int>(text.size()));
      memcpy(buf, text.c_str(), static_cast<size_t>(copy));
      buf[copy] = '\0';
    }
    return need;
  }

  static neui_widget_t NEUI_ABI w_get_first_child(neui_session_t session,
                                                    neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return { UINT32_MAX };
    uint32_t idx = WidgetToIndex(widget);
    uint32_t child = s->_widgets.child(idx);
    return child ? IndexToWidget(s->_session_id, child) : neui_widget_t{ UINT32_MAX };
  }

  static neui_widget_t NEUI_ABI w_get_next_sibling(neui_session_t session,
                                                     neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return { UINT32_MAX };
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return { UINT32_MAX };
    uint32_t next = s->_widgets.next(idx);
    return next ? IndexToWidget(s->_session_id, next) : neui_widget_t{ UINT32_MAX };
  }

  static void NEUI_ABI w_set_focus(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    s->set_focus(WidgetToIndex(widget));
  }

  static void NEUI_ABI w_set_check(neui_session_t session,
                                    neui_widget_t widget,
                                    neui_check_state_t state)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto* cb = dynamic_cast<CheckboxWidget*>(&s->_widgets[idx]);
    if (cb) cb->check_state = static_cast<int>(state);
  }

  static neui_check_state_t NEUI_ABI w_get_check(neui_session_t session,
                                                   neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return NEUI_CHECK_UNCHECKED;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return NEUI_CHECK_UNCHECKED;
    auto* cb = dynamic_cast<CheckboxWidget*>(&s->_widgets[idx]);
    if (!cb) return NEUI_CHECK_UNCHECKED;
    return static_cast<neui_check_state_t>(cb->check_state);
  }

  static void* NEUI_ABI w_get_native_handle(neui_session_t session,
                                              neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return nullptr;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return nullptr;
    return s->_widgets[idx].native_handle;
  }

  static void NEUI_ABI w_set_tab_stop(neui_session_t session,
                                       neui_widget_t widget, bool enabled)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (s->_widgets.exists(idx))
      s->_widgets[idx].tab_stop = enabled;
  }

  static void NEUI_ABI w_set_enabled(neui_session_t session,
                                      neui_widget_t widget, bool enabled)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];
    if (wd.enabled == enabled) return;
    wd.enabled = enabled;
    // If the disabled widget currently held focus, move focus to the next
    // tab-stop so keyboard input stays usable.
    if (!enabled && s->_focused_widget == idx)
      s->focus_next(true);
    // Repaint so the dim alpha bracket picks up the new state.
    if (void* frame = s->find_parent_native_handle(idx))
      platform_invalidate(frame);
  }

  static bool NEUI_ABI w_get_enabled(neui_session_t session,
                                      neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return false;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return false;
    return s->_widgets[idx].enabled;
  }

  static void NEUI_ABI w_set_owner(neui_session_t session,
                                     neui_widget_t dialog, neui_widget_t owner)
  {
    auto* s = get_session_for_widget(session, dialog);
    if (!s) return;
    if (!widget_belongs_to_session(owner, s->_session_id)) return;
    s->widget_set_owner(dialog, owner);
  }

  static void NEUI_ABI w_get_pos(neui_session_t session, neui_widget_t widget,
                                  int* x, int* y)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];
    if (x) *x = wd.x;
    if (y) *y = wd.y;
  }

  static void NEUI_ABI w_get_size(neui_session_t session, neui_widget_t widget,
                                   int* width, int* height)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];
    if (width)  *width  = wd.width;
    if (height) *height = wd.height;
  }

  static void NEUI_ABI w_get_client_rect(neui_session_t session, neui_widget_t widget,
                                         int* x, int* y, int* width, int* height)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    s->widget_client_rect(WidgetToIndex(widget), x, y, width, height);
  }

  static int NEUI_ABI w_popup_menu(neui_session_t session, neui_widget_t anchor,
                                    int x, int y, const char* const* items)
  {
    auto* s = get_session_for_widget(session, anchor);
    if (!s) return 0;
    uint32_t aidx = WidgetToIndex(anchor);
    if (!s->_widgets.exists(aidx)) return 0;
    if (!items) return 0;
    std::vector<std::string> v;
    for (int i = 0; items[i] != nullptr; ++i) v.emplace_back(items[i]);
    return s->open_popup_menu(aidx, x, y, v);
  }

  static void NEUI_ABI w_invalidate(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    // Map any widget invalidation to a frame-level repaint - on the xpl
    // host the entire frame paints in one pass, so per-widget invalidation
    // would have to invalidate the frame anyway.
    if (void* frame = s->find_parent_native_handle(idx))
      platform_invalidate(frame);
  }

  neui_widget_api_t widgets_api = {
    w_create,
    w_destroy,
    w_show,
    w_hide,
    w_set_pos,
    w_set_size,
    w_set_emit_events,
    w_set_text,
    w_get_text,
    w_get_first_child,
    w_get_next_sibling,
    w_set_focus,
    w_set_check,
    w_get_check,
    w_get_native_handle,
    w_set_tab_stop,
    w_set_owner,
    w_get_pos,
    w_get_size,
    w_popup_menu,
    w_invalidate,
    w_set_asset,
    w_set_enabled,
    w_get_enabled,
    w_get_client_rect,
    w_create_from_component,
  };

  // -------------------------------------------------------------------------
  // Attribute API

  static int NEUI_ABI a_set_int(neui_session_t session, neui_widget_t widget,
                                 const char* key, int32_t value)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    auto& wd = s->_widgets[idx];
    neui_detail::ensure_attrs(wd.attrs).set_int(key, value);

    // Live-update of size constraints on frames. Win32 reads attrs directly
    // in WM_GETMINMAXINFO so its impl is a no-op; macOS pushes via
    // setContentMin/MaxSize: at the call site.
    if (wd.is_frame() && wd.native_handle &&
        (!strcmp(key, NEUI_ATTR_MIN_WIDTH)  ||
         !strcmp(key, NEUI_ATTR_MIN_HEIGHT) ||
         !strcmp(key, NEUI_ATTR_MAX_WIDTH)  ||
         !strcmp(key, NEUI_ATTR_MAX_HEIGHT)))
    {
      int min_w = wd.attrs ? wd.attrs->get_int(NEUI_ATTR_MIN_WIDTH,  0) : 0;
      int min_h = wd.attrs ? wd.attrs->get_int(NEUI_ATTR_MIN_HEIGHT, 0) : 0;
      int max_w = wd.attrs ? wd.attrs->get_int(NEUI_ATTR_MAX_WIDTH,  0) : 0;
      int max_h = wd.attrs ? wd.attrs->get_int(NEUI_ATTR_MAX_HEIGHT, 0) : 0;
      platform_apply_size_constraints(wd.native_handle,
                                       min_w, min_h, max_w, max_h);
    }
    // Self-painted widgets read int attrs each paint (NEUI_ATTR_BACKGROUND,
    // NEUI_ATTR_TRISTATE, NEUI_ATTR_STEPS etc.), so a runtime change has
    // to invalidate the owning frame. Frames handle their own side
    // effects above (size constraints).
    if (!wd.is_frame()) {
      if (void* frame = s->find_parent_native_handle(idx))
        platform_invalidate(frame);
    }
    return 1;
  }

  static int32_t NEUI_ABI a_get_int(neui_session_t session, neui_widget_t widget,
                                     const char* key, int32_t default_value)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return default_value;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return default_value;
    const auto& wd = s->_widgets[idx];
    if (!wd.attrs) return default_value;
    return wd.attrs->get_int(key, default_value);
  }

  static int NEUI_ABI a_set_string(neui_session_t session, neui_widget_t widget,
                                    const char* key, const char* value)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    auto& wd = s->_widgets[idx];
    neui_detail::ensure_attrs(wd.attrs).set_string(key, value);

    // Live re-application for behavior-bearing keys. Each platform layer's
    // platform_set_window_icon does the right thing: Win32 manages the
    // owned HICON via wd.native_icon; macOS sets NSApp.applicationIconImage.
    if (wd.is_frame() && wd.native_handle &&
        !strcmp(key, NEUI_ATTR_ICON_PATH))
    {
      platform_set_window_icon(wd, value);
    }
    // Self-painted widgets read string attrs (e.g. NEUI_ATTR_ALIGN_TEXT
    // on SECTION, NEUI_ATTR_VALUE_TEXT on KNOB) each paint, so a runtime
    // change has to invalidate the owning frame. Frames go through the
    // icon_path branch above for their one live-update key; everything
    // else just invalidates the parent frame.
    if (!wd.is_frame()) {
      if (void* frame = s->find_parent_native_handle(idx))
        platform_invalidate(frame);
    }
    return 1;
  }

  static const char* NEUI_ABI a_get_string(neui_session_t session,
                                            neui_widget_t widget, const char* key)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return nullptr;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return nullptr;
    const auto& wd = s->_widgets[idx];
    if (!wd.attrs) return nullptr;
    return wd.attrs->get_string(key);
  }

  static int NEUI_ABI a_has(neui_session_t session, neui_widget_t widget,
                             const char* key)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    const auto& wd = s->_widgets[idx];
    return (wd.attrs && wd.attrs->has(key)) ? 1 : 0;
  }

  static int NEUI_ABI a_remove(neui_session_t session, neui_widget_t widget,
                                const char* key)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    auto& wd = s->_widgets[idx];
    if (!wd.attrs) return 0;
    return wd.attrs->remove(key) ? 1 : 0;
  }

  static int NEUI_ABI a_set_float(neui_session_t session, neui_widget_t widget,
                                   const char* key, float value)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    auto& wd = s->_widgets[idx];

    // Clamp + snap NEUI_PARAM_VALUE on entry so attribute storage and
    // anything reading it back (paint, mouse hit-testing) stay coherent.
    float stored = value;
    if (!strcmp(key, NEUI_PARAM_VALUE)) {
      if (stored < 0.0f) stored = 0.0f;
      if (stored > 1.0f) stored = 1.0f;
      int steps = wd.attrs ? wd.attrs->get_int(NEUI_ATTR_STEPS, 0) : 0;
      if (steps >= 2) {
        int sidx = static_cast<int>(stored * static_cast<float>(steps - 1) + 0.5f);
        if (sidx < 0) sidx = 0;
        if (sidx >= steps) sidx = steps - 1;
        stored = static_cast<float>(sidx) / static_cast<float>(steps - 1);
      }
    }
    neui_detail::ensure_attrs(wd.attrs).set_float(key, stored);
    // Float attrs feed live paint state (NEUI_PARAM_VALUE on KNOB /
    // SLIDER, NEUI_ATTR_ROTATION on IMAGE, etc.). Invalidate the owning
    // frame so the next paint pulls fresh values. Frames don't currently
    // read any float attr in paint, but skip them for symmetry.
    if (!wd.is_frame()) {
      if (void* frame = s->find_parent_native_handle(idx))
        platform_invalidate(frame);
    }
    return 1;
  }

  static float NEUI_ABI a_get_float(neui_session_t session, neui_widget_t widget,
                                     const char* key, float default_value)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return default_value;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return default_value;
    const auto& wd = s->_widgets[idx];
    if (!wd.attrs) return default_value;
    return wd.attrs->get_float(key, default_value);
  }

  static int NEUI_ABI a_set_session_int(neui_session_t session,
                                         const char* key, int32_t value)
  {
    auto* s = get_session(session);
    if (!s || !key) return 0;
    s->_session_attrs.set_int(key, value);
    if (!strcmp(key, NEUI_ATTR_THEME_MODE)) {
      // Live-apply: recompute the effective palette, refresh the frozen
      // snapshot, and repaint every frame (the user explicitly switched
      // modes, so even FOLLOW=0 frames need to update).
      s->on_theme_changed(true);
    }
    return 1;
  }

  static int32_t NEUI_ABI a_get_session_int(neui_session_t session,
                                             const char* key,
                                             int32_t default_value)
  {
    auto* s = get_session(session);
    if (!s || !key) return default_value;
    return s->_session_attrs.get_int(key, default_value);
  }

  neui_attr_api_t attrs_api = {
    NEUI_VERSION,
    a_set_int,
    a_get_int,
    a_set_string,
    a_get_string,
    a_has,
    a_remove,
    a_set_float,
    a_get_float,
    a_set_session_int,
    a_get_session_int,
  };

  // -------------------------------------------------------------------------
  // Items API (ListItemsWidget)

  static void NEUI_ABI i_clear(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto* lb = dynamic_cast<ListItemsWidget*>(&s->_widgets[idx]);
    if (lb) lb->items.clear();
  }

  static uint32_t NEUI_ABI i_add(neui_session_t session, neui_widget_t widget,
                                  const char* text, void* userdata)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return UINT32_MAX;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return UINT32_MAX;
    auto* lb = dynamic_cast<ListItemsWidget*>(&s->_widgets[idx]);
    if (!lb) return UINT32_MAX;
    ListItemsWidget::Item item;
    item.text = text ? text : "";
    item.userdata = userdata;
    lb->items.push_back(std::move(item));
    return static_cast<uint32_t>(lb->items.size() - 1);
  }

  static void NEUI_ABI i_remove(neui_session_t session, neui_widget_t widget,
                                 uint32_t index)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto* lb = dynamic_cast<ListItemsWidget*>(&s->_widgets[idx]);
    if (!lb) return;
    if (index < lb->items.size())
      lb->items.erase(lb->items.begin() + index);
  }

  static uint32_t NEUI_ABI i_count(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    auto* lb = dynamic_cast<ListItemsWidget*>(&s->_widgets[idx]);
    if (!lb) return 0;
    return static_cast<uint32_t>(lb->items.size());
  }

  static int NEUI_ABI i_get_text(neui_session_t session, neui_widget_t widget,
                                  uint32_t index, char* buf, int buflen)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return -1;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return -1;
    auto* lb = dynamic_cast<ListItemsWidget*>(&s->_widgets[idx]);
    if (!lb || index >= lb->items.size()) return -1;
    const auto& text = lb->items[index].text;
    int need = static_cast<int>(text.size()) + 1;
    if (buf && buflen > 0) {
      int copy = std::min(buflen - 1, static_cast<int>(text.size()));
      memcpy(buf, text.c_str(), static_cast<size_t>(copy));
      buf[copy] = '\0';
    }
    return need;
  }

  static void NEUI_ABI i_set_text(neui_session_t session, neui_widget_t widget,
                                   uint32_t index, const char* text)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto* lb = dynamic_cast<ListItemsWidget*>(&s->_widgets[idx]);
    if (!lb) return;
    if (index < lb->items.size())
      lb->items[index].text = text ? text : "";
  }

  static void* NEUI_ABI i_get_userdata(neui_session_t session,
                                        neui_widget_t widget, uint32_t index)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return nullptr;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return nullptr;
    auto* lb = dynamic_cast<ListItemsWidget*>(&s->_widgets[idx]);
    if (!lb) return nullptr;
    return index < lb->items.size() ? lb->items[index].userdata : nullptr;
  }

  static uint32_t NEUI_ABI i_get_selected(neui_session_t session,
                                            neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return UINT32_MAX;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return UINT32_MAX;
    auto* lb = dynamic_cast<ListItemsWidget*>(&s->_widgets[idx]);
    if (!lb) return UINT32_MAX;
    return lb->selected_item;
  }

  static void NEUI_ABI i_set_selected(neui_session_t session,
                                       neui_widget_t widget, uint32_t index)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto* lb = dynamic_cast<ListItemsWidget*>(&s->_widgets[idx]);
    if (lb) lb->selected_item = index;
  }

  neui_items_api_t items_api = {
    i_clear,
    i_add,
    i_remove,
    i_count,
    i_get_text,
    i_set_text,
    i_get_userdata,
    i_get_selected,
    i_set_selected,
  };

  // -------------------------------------------------------------------------
  // Tree API (MenubarWidget and TreeviewWidget)

  static std::string make_menu_text(const char* text, const char* shortcut)
  {
    std::string s = text ? text : "";
    if (shortcut && *shortcut) { s += '\t'; s += shortcut; }
    return s;
  }

  static neui_item_t NEUI_ABI t_add(neui_session_t session,
                                     neui_widget_t widget,
                                     neui_item_t parent,
                                     const char* text, void* userdata)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return { UINT32_MAX };
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return { UINT32_MAX };
    auto& wd = s->_widgets[idx];

    // ---- menubar ----
    if (wd.is_menubar()) {
      auto& mb = dynamic_cast<MenubarWidget&>(wd);
      if (!mb.hmenu) return { UINT32_MAX };
      uint32_t neui_id = mb.next_menu_item_id++;
      MenubarWidget::MenuItemData data;
      data.text           = text ? text : "";
      data.enabled        = true;
      data.userdata       = userdata;
      data.parent_item_id = parent.id;

      if (parent.id == 0) {
        void* popup = platform_menubar_add_popup(mb.hmenu, data.text.c_str());
        data.parent_hmenu = mb.hmenu;
        data.submenu      = popup;
        data.cmd_id       = 0;
      } else {
        auto pit = mb.menu_items.find(parent.id);
        void* parent_popup = (pit != mb.menu_items.end() && pit->second.submenu)
                             ? pit->second.submenu
                             : mb.hmenu;
        uint32_t cmd_id = mb.next_menu_cmd_id++;
        data.parent_hmenu = parent_popup;
        data.submenu      = nullptr;
        data.cmd_id       = cmd_id;

        if (strcmp(data.text.c_str(), "-") == 0) {
          data.is_separator = true;
          platform_menubar_add_separator(parent_popup, cmd_id);
        } else {
          platform_menubar_add_item(parent_popup, cmd_id, data.text.c_str());
          mb.menu_cmd_map[cmd_id] = neui_id;
        }
      }

      mb.menu_items[neui_id] = std::move(data);
      mb.menu_item_ids_ordered.push_back(neui_id);

      void* frame = s->find_parent_native_handle(idx);
      if (frame) platform_menubar_refresh(frame);
      return { neui_id };
    }

    // ---- treeview ----
    auto& tv = dynamic_cast<TreeviewWidget&>(wd);
    uint32_t id = tv.next_tree_id++;
    TreeviewWidget::TreeItem item;
    item.parent_id = parent.id;
    item.text      = text ? text : "";
    item.userdata  = userdata;
    tv.tree_items[id] = std::move(item);
    tv.tree_items_ordered.push_back(id);
    return { id };
  }

  // Forward decl - body is below near t_set_shortcut.
  static void rebuild_menubar_accel(MenubarWidget& mb);

  static void NEUI_ABI t_remove(neui_session_t session,
                                 neui_widget_t widget, neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];

    if (wd.is_menubar()) {
      auto& mb = dynamic_cast<MenubarWidget&>(wd);
      auto it = mb.menu_items.find(item.id);
      if (it == mb.menu_items.end()) return;
      auto& data = it->second;
      if (data.submenu) {
        platform_menubar_remove_popup(data.parent_hmenu, data.submenu);
      } else {
        platform_menubar_remove_item(data.parent_hmenu, data.cmd_id);
        mb.menu_cmd_map.erase(data.cmd_id);
      }
      bool had_shortcut = (data.shortcut_key != NEUI_KEY_NONE);
      mb.menu_item_ids_ordered.erase(
        std::remove(mb.menu_item_ids_ordered.begin(),
                    mb.menu_item_ids_ordered.end(), item.id),
        mb.menu_item_ids_ordered.end());
      mb.menu_items.erase(it);
      if (had_shortcut) rebuild_menubar_accel(mb);
      void* frame = s->find_parent_native_handle(idx);
      if (frame) platform_menubar_refresh(frame);
      return;
    }

    auto& tv = dynamic_cast<TreeviewWidget&>(wd);
    tv.tree_items.erase(item.id);
    tv.tree_items_ordered.erase(
      std::remove(tv.tree_items_ordered.begin(), tv.tree_items_ordered.end(),
                  item.id),
      tv.tree_items_ordered.end());
  }

  static void NEUI_ABI t_clear(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];

    if (wd.is_menubar()) {
      auto& mb = dynamic_cast<MenubarWidget&>(wd);
      for (auto& kv : mb.menu_items) {
        if (kv.second.submenu)
          platform_menubar_destroy(kv.second.submenu);
      }
      mb.menu_items.clear();
      mb.menu_cmd_map.clear();
      mb.menu_item_ids_ordered.clear();
      mb.next_menu_item_id = 1;
      mb.next_menu_cmd_id  = 0x8000;
      // Drop the accelerator table - no items, no shortcuts.
#ifdef _WIN32
      if (mb.native_accel) {
        DestroyAcceleratorTable(static_cast<HACCEL>(mb.native_accel));
        mb.native_accel = nullptr;
      }
#else
      // TODO(macos): NSMenuItem.keyEquivalent is set per-item; nothing
      // to tear down here. See plans/macos-port.md.
      mb.native_accel = nullptr;
#endif
      platform_menubar_destroy(mb.hmenu);
      mb.hmenu = platform_menubar_create(IndexToWidget(s->_session_id, idx).id);
      void* frame = s->find_parent_native_handle(idx);
      if (frame) platform_menubar_attach(frame, mb.hmenu);
      return;
    }

    auto& tv = dynamic_cast<TreeviewWidget&>(wd);
    tv.tree_items.clear();
    tv.tree_items_ordered.clear();
    tv.next_tree_id = 1;
  }

  static int NEUI_ABI t_get_text(neui_session_t session, neui_widget_t widget,
                                  neui_item_t item, char* buf, int buflen)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return -1;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return -1;
    auto& wd = s->_widgets[idx];

    const std::string* text = nullptr;
    if (wd.is_menubar()) {
      auto& mb = dynamic_cast<MenubarWidget&>(wd);
      auto it = mb.menu_items.find(item.id);
      if (it == mb.menu_items.end()) return -1;
      text = &it->second.text;
    } else {
      auto& tv = dynamic_cast<TreeviewWidget&>(wd);
      auto it = tv.tree_items.find(item.id);
      if (it == tv.tree_items.end()) return -1;
      text = &it->second.text;
    }

    int need = static_cast<int>(text->size()) + 1;
    if (buf && buflen > 0) {
      int copy = std::min(buflen - 1, static_cast<int>(text->size()));
      memcpy(buf, text->c_str(), static_cast<size_t>(copy));
      buf[copy] = '\0';
    }
    return need;
  }

  static void NEUI_ABI t_set_text(neui_session_t session, neui_widget_t widget,
                                   neui_item_t item, const char* text)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];

    if (wd.is_menubar()) {
      auto& mb = dynamic_cast<MenubarWidget&>(wd);
      auto it = mb.menu_items.find(item.id);
      if (it == mb.menu_items.end()) return;
      it->second.text = text ? text : "";
      if (!it->second.submenu && !it->second.is_separator) {
        std::string dt = make_menu_text(it->second.text.c_str(),
                                        it->second.shortcut.c_str());
        platform_menubar_set_item_text(it->second.parent_hmenu,
                                       it->second.cmd_id, dt.c_str());
        void* frame = s->find_parent_native_handle(idx);
        if (frame) platform_menubar_refresh(frame);
      }
      return;
    }

    auto& tv = dynamic_cast<TreeviewWidget&>(wd);
    auto it = tv.tree_items.find(item.id);
    if (it != tv.tree_items.end())
      it->second.text = text ? text : "";
  }

  static void* NEUI_ABI t_get_userdata(neui_session_t session,
                                        neui_widget_t widget, neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return nullptr;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return nullptr;
    auto& wd = s->_widgets[idx];

    if (wd.is_menubar()) {
      auto& mb = dynamic_cast<MenubarWidget&>(wd);
      auto it = mb.menu_items.find(item.id);
      return it != mb.menu_items.end() ? it->second.userdata : nullptr;
    }
    auto& tv = dynamic_cast<TreeviewWidget&>(wd);
    auto it = tv.tree_items.find(item.id);
    return it != tv.tree_items.end() ? it->second.userdata : nullptr;
  }

  static void NEUI_ABI t_set_enabled(neui_session_t session,
                                      neui_widget_t widget,
                                      neui_item_t item, bool enabled)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];

    if (wd.is_menubar()) {
      auto& mb = dynamic_cast<MenubarWidget&>(wd);
      auto it = mb.menu_items.find(item.id);
      if (it == mb.menu_items.end()) return;
      it->second.enabled = enabled;
      if (it->second.submenu) {
        platform_menubar_enable_popup(it->second.parent_hmenu,
                                      it->second.submenu, enabled);
      } else {
        platform_menubar_enable_item(it->second.parent_hmenu,
                                     it->second.cmd_id, enabled);
      }
      void* frame = s->find_parent_native_handle(idx);
      if (frame) platform_menubar_refresh(frame);
      return;
    }

    auto& tv = dynamic_cast<TreeviewWidget&>(wd);
    auto it = tv.tree_items.find(item.id);
    if (it != tv.tree_items.end())
      it->second.enabled = enabled;
  }

  static bool NEUI_ABI t_get_enabled(neui_session_t session,
                                      neui_widget_t widget, neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return false;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return false;
    auto& wd = s->_widgets[idx];

    if (wd.is_menubar()) {
      auto& mb = dynamic_cast<MenubarWidget&>(wd);
      auto it = mb.menu_items.find(item.id);
      return it != mb.menu_items.end() && it->second.enabled;
    }
    auto& tv = dynamic_cast<TreeviewWidget&>(wd);
    auto it = tv.tree_items.find(item.id);
    return it != tv.tree_items.end() && it->second.enabled;
  }

  bool Session::try_translate_accel(void* msg_ptr)
  {
#ifdef _WIN32
    auto* msg = static_cast<MSG*>(msg_ptr);
    if (!msg) return false;
    HWND root = msg->hwnd ? GetAncestor(msg->hwnd, GA_ROOT) : nullptr;
    if (!root) return false;
    for (uint32_t mb_idx : _menubars) {
      if (!_widgets.exists(mb_idx)) continue;
      auto* mb = dynamic_cast<MenubarWidget*>(&_widgets[mb_idx]);
      if (!mb || !mb->native_accel) continue;
      void* frame = find_parent_native_handle(mb_idx);
      if (!frame || frame != static_cast<void*>(root)) continue;
      if (TranslateAcceleratorW(root,
                                static_cast<HACCEL>(mb->native_accel), msg))
        return true;
    }
    return false;
#else
    // macOS: NSMenuItem.keyEquivalent triggers the action selector directly,
    // there's no separate accel-table to translate. The platform layer hands
    // the keystroke to the menu before the responder chain. See plans/macos-port.md.
    (void)msg_ptr;
    return false;
#endif
  }

  // Rebuild the menubar's accelerator table from its current item shortcuts.
  // Win32: builds an HACCEL the message pump translates. macOS: per-item
  // NSMenuItem.keyEquivalent - set by t_set_shortcut directly via a future
  // platform_menubar_set_item_shortcut, no centralised rebuild needed.
  static void rebuild_menubar_accel(MenubarWidget& mb)
  {
#ifdef _WIN32
    std::vector<neui_detail::AccelEntry> entries;
    entries.reserve(mb.menu_items.size());
    for (const auto& kv : mb.menu_items) {
      const auto& d = kv.second;
      if (d.submenu || d.is_separator) continue;
      if (d.shortcut_key != NEUI_KEY_NONE)
        entries.push_back({ d.shortcut_mods, d.shortcut_key, d.cmd_id });
      // Standard platform-alias shortcuts (e.g. Ctrl+Shift+Z for REDO).
      neui_detail::append_builtin_command_aliases(
        d.menu_cmd, d.shortcut_mods, d.shortcut_key, d.cmd_id, entries);
    }
    HACCEL old = static_cast<HACCEL>(mb.native_accel);
    HACCEL neu = neui_detail::build_accel_table(entries);
    mb.native_accel = neu;
    if (old) DestroyAcceleratorTable(old);
#else
    (void)mb;
#endif
  }

  static void NEUI_ABI t_set_shortcut(neui_session_t session,
                                       neui_widget_t widget,
                                       neui_item_t item,
                                       uint32_t modifiers, uint32_t key)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];

    if (wd.is_menubar()) {
      auto& mb = dynamic_cast<MenubarWidget&>(wd);
      auto it = mb.menu_items.find(item.id);
      if (it == mb.menu_items.end()) return;
      it->second.shortcut_mods = modifiers;
      it->second.shortcut_key  = key;
      it->second.shortcut      =
        neui_detail::format_shortcut_label_win(modifiers, key);
      if (!it->second.submenu && !it->second.is_separator) {
        std::string dt = make_menu_text(it->second.text.c_str(),
                                        it->second.shortcut.c_str());
        platform_menubar_set_item_text(it->second.parent_hmenu,
                                       it->second.cmd_id, dt.c_str());
        // Per-item shortcut binding. Win32 ignores it (HACCEL is rebuilt
        // centrally); macOS uses it to set NSMenuItem.keyEquivalent +
        // keyEquivalentModifierMask. Always called on every platform -
        // the no-op on Win32 keeps the call site uniform.
        platform_menubar_set_item_shortcut(it->second.parent_hmenu,
                                            it->second.cmd_id,
                                            modifiers, key);
        void* frame = s->find_parent_native_handle(idx);
        if (frame) platform_menubar_refresh(frame);
      }
      rebuild_menubar_accel(mb);
      return;
    }
    // Treeview: shortcut is ignored (no accelerator semantics for tree items).
  }

  static neui_item_t NEUI_ABI t_get_first_child(neui_session_t session,
                                                  neui_widget_t widget,
                                                  neui_item_t parent)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return { UINT32_MAX };
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return { UINT32_MAX };
    auto* tv = dynamic_cast<TreeviewWidget*>(&s->_widgets[idx]);
    if (!tv) return { UINT32_MAX };
    // Walk the insertion-ordered list to preserve add() order.
    for (uint32_t id : tv->tree_items_ordered) {
      auto it = tv->tree_items.find(id);
      if (it != tv->tree_items.end() && it->second.parent_id == parent.id)
        return { id };
    }
    return { UINT32_MAX };
  }

  static neui_item_t NEUI_ABI t_get_next_sibling(neui_session_t session,
                                                   neui_widget_t widget,
                                                   neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return { UINT32_MAX };
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return { UINT32_MAX };
    auto* tv = dynamic_cast<TreeviewWidget*>(&s->_widgets[idx]);
    if (!tv) return { UINT32_MAX };
    auto it = tv->tree_items.find(item.id);
    if (it == tv->tree_items.end()) return { UINT32_MAX };
    uint32_t parent_id = it->second.parent_id;
    bool found_self = false;
    for (uint32_t id : tv->tree_items_ordered) {
      if (id == item.id) { found_self = true; continue; }
      if (!found_self) continue;
      auto sit = tv->tree_items.find(id);
      if (sit != tv->tree_items.end() && sit->second.parent_id == parent_id)
        return { id };
    }
    return { UINT32_MAX };
  }

  static neui_item_t NEUI_ABI t_get_selected(neui_session_t session,
                                               neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return { UINT32_MAX };
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return { UINT32_MAX };
    auto* tv = dynamic_cast<TreeviewWidget*>(&s->_widgets[idx]);
    if (!tv) return { UINT32_MAX };
    return { tv->selected_tree_item };
  }

  static void NEUI_ABI t_set_selected(neui_session_t session,
                                       neui_widget_t widget, neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto* tv = dynamic_cast<TreeviewWidget*>(&s->_widgets[idx]);
    if (tv) tv->selected_tree_item = item.id;
  }

  static void NEUI_ABI t_set_menu_cmd(neui_session_t session,
                                       neui_widget_t widget,
                                       neui_item_t item, uint32_t command)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];
    if (!wd.is_menubar()) return;   // treeview ignores menu_cmd
    auto& mb = dynamic_cast<MenubarWidget&>(wd);
    auto it = mb.menu_items.find(item.id);
    if (it != mb.menu_items.end()) it->second.menu_cmd = command;
  }

  neui_tree_api_t tree_api = {
    t_add,
    t_remove,
    t_clear,
    t_get_text,
    t_set_text,
    t_get_userdata,
    t_set_enabled,
    t_get_enabled,
    t_set_shortcut,
    t_get_first_child,
    t_get_next_sibling,
    t_get_selected,
    t_set_selected,
    t_set_menu_cmd,
  };

  // -------------------------------------------------------------------------
  // Clipboard API

  static int NEUI_ABI cb_set_text(neui_session_t session, const char* utf8)
  {
    (void)session;
    if (!utf8) return 0;
    return platform_clipboard_set_text(
             utf8, static_cast<uint32_t>(strlen(utf8))) ? 1 : 0;
  }

  static int NEUI_ABI cb_get_text(neui_session_t session, char* buf, int buflen)
  {
    (void)session;
    return platform_clipboard_get_text(buf, buflen);
  }

  static bool NEUI_ABI cb_has_text(neui_session_t session)
  {
    (void)session;
    return platform_clipboard_has_text();
  }

  static neui_data_item_t NEUI_ABI cb_read(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return neui_data_item_none;
    uint32_t id = s->_data_items.allocate();
    auto* item = s->_data_items.get(id);
    if (!item) return neui_data_item_none;
    if (!platform_clipboard_read_item(*item)) {
      s->_data_items.release(id);
      return neui_data_item_none;
    }
    return { id };
  }

  static neui_data_item_t NEUI_ABI cb_create_item(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return neui_data_item_none;
    return { s->_data_items.allocate() };
  }

  static void NEUI_ABI cb_release(neui_session_t session,
                                   neui_data_item_t item)
  {
    auto* s = get_session(session);
    if (!s) return;
    s->_data_items.release(item.id);
  }

  static int NEUI_ABI cb_write(neui_session_t session,
                                neui_data_item_t item)
  {
    auto* s = get_session(session);
    if (!s) return 0;
    auto* it = s->_data_items.get(item.id);
    if (!it) return 0;
    return platform_clipboard_write_item(*it) ? 1 : 0;
  }

  static int NEUI_ABI cb_item_set_format(neui_session_t session,
                                          neui_data_item_t item,
                                          const char* mime,
                                          const void* data, uint32_t length)
  {
    auto* s = get_session(session);
    if (!s || !mime) return 0;
    auto* it = s->_data_items.get(item.id);
    if (!it) return 0;
    it->set_format(mime, data, length);
    return 1;
  }

  static int NEUI_ABI cb_item_get_format(neui_session_t session,
                                          neui_data_item_t item,
                                          const char* mime,
                                          void* buf, int buflen)
  {
    auto* s = get_session(session);
    if (!s || !mime) return -1;
    auto* it = s->_data_items.get(item.id);
    if (!it) return -1;
    return it->get_format(mime, buf, buflen);
  }

  static bool NEUI_ABI cb_item_has_format(neui_session_t session,
                                           neui_data_item_t item,
                                           const char* mime)
  {
    auto* s = get_session(session);
    if (!s || !mime) return false;
    auto* it = s->_data_items.get(item.id);
    return it && it->has_format(mime);
  }

  static int NEUI_ABI cb_item_set_format_callback(neui_session_t session,
                                                   neui_data_item_t item,
                                                   const char* mime,
                                                   neui_data_provider_t provider,
                                                   void* userdata)
  {
    auto* s = get_session(session);
    if (!s || !mime || !provider) return 0;
    auto* it = s->_data_items.get(item.id);
    if (!it) return 0;
    it->set_format_provider(mime, provider, userdata);
    return 1;
  }

  neui_clipboard_api_t clipboard_api = {
    NEUI_VERSION,
    cb_set_text,
    cb_get_text,
    cb_has_text,
    cb_read,
    cb_create_item,
    cb_release,
    cb_write,
    cb_item_set_format,
    cb_item_get_format,
    cb_item_has_format,
    cb_item_set_format_callback,
  };

  // -------------------------------------------------------------------------
  // Commands API

  bool Session::invoke_focused_command(uint32_t cmd)
  {
    if (cmd == NEUI_CMD_NONE || cmd >= NEUI_CMD_USER_BASE) return false;
    if (_focused_widget == 0 || !_widgets.exists(_focused_widget)) return false;
    return _widgets[_focused_widget].perform_command(cmd);
  }

  bool Session::invoke_command(neui_widget_t widget, uint32_t cmd)
  {
    if (cmd == NEUI_CMD_NONE || cmd >= NEUI_CMD_USER_BASE) return false;
    uint32_t idx = WidgetToIndex(widget);
    if (!_widgets.exists(idx)) return false;
    return _widgets[idx].perform_command(cmd);
  }

  bool Session::can_focused_perform_command(uint32_t cmd)
  {
    if (cmd == NEUI_CMD_NONE || cmd >= NEUI_CMD_USER_BASE) return false;
    if (_focused_widget == 0 || !_widgets.exists(_focused_widget)) return false;
    return _widgets[_focused_widget].can_perform_command(cmd);
  }

  static int NEUI_ABI cmd_invoke_focused(neui_session_t session, uint32_t cmd)
  {
    auto* s = get_session(session);
    return (s && s->invoke_focused_command(cmd)) ? 1 : 0;
  }

  static int NEUI_ABI cmd_invoke(neui_session_t session, neui_widget_t widget,
                                  uint32_t cmd)
  {
    auto* s = get_session_for_widget(session, widget);
    return (s && s->invoke_command(widget, cmd)) ? 1 : 0;
  }

  neui_commands_api_t commands_api = {
    NEUI_VERSION,
    cmd_invoke_focused,
    cmd_invoke,
  };

  // ---------------------------------------------------------------------------
  // Scroll API (NEUI_API_SCROLL)
  // ---------------------------------------------------------------------------

  static int NEUI_ABI scroll_set(neui_session_t session, neui_widget_t widget,
                                  int scroll_x, int scroll_y)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    auto* sec = dynamic_cast<SectionWidget*>(&s->_widgets[idx]);
    if (!sec || !sec->scroll_state) return 0;
    sec->external_commit(scroll_x, scroll_y);
    return 1;
  }

  static int NEUI_ABI scroll_get(neui_session_t session, neui_widget_t widget,
                                  int* out_x, int* out_y)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    auto* sec = dynamic_cast<SectionWidget*>(&s->_widgets[idx]);
    if (!sec || !sec->scroll_state) return 0;
    if (out_x) *out_x = sec->scroll_state->scroll_x;
    if (out_y) *out_y = sec->scroll_state->scroll_y;
    return 1;
  }

  static int NEUI_ABI scroll_ensure_visible(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    s->ensure_widget_visible(idx);
    return 1;
  }

  neui_scroll_api_t scroll_api = {
    NEUI_VERSION,
    scroll_set,
    scroll_get,
    scroll_ensure_visible,
  };

  // ---------------------------------------------------------------------------
  // Tabs API (NEUI_API_TABS) - selection control over a TABVIEW. Tabs are the
  // TABVIEW's NEUI_W_TABPAGE children in creation order.
  // ---------------------------------------------------------------------------

  static TabViewWidget* tabview_from(neui_session_t session, neui_widget_t widget,
                                     Session** out_s)
  {
    auto* s = get_session_for_widget(session, widget);
    if (out_s) *out_s = s;
    if (!s) return nullptr;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return nullptr;
    return dynamic_cast<TabViewWidget*>(&s->_widgets[idx]);
  }

  static uint32_t NEUI_ABI tabs_count(neui_session_t session, neui_widget_t widget)
  {
    auto* tv = tabview_from(session, widget, nullptr);
    if (!tv) return 0;
    std::vector<uint32_t> pages; tv->collect_pages(pages);
    return static_cast<uint32_t>(pages.size());
  }

  static uint32_t NEUI_ABI tabs_get_selected(neui_session_t session, neui_widget_t widget)
  {
    auto* tv = tabview_from(session, widget, nullptr);
    if (!tv) return NEUI_ITEM_NONE;
    std::vector<uint32_t> pages; tv->collect_pages(pages);
    if (pages.empty()) return NEUI_ITEM_NONE;
    // Clamp defensively: tv->selected is normally re-clamped on select / paint /
    // page-destroy, but a programmatic set_selected before the first paint can
    // leave it out of range. Matches the win32 host's tabs_get_selected.
    int sel = tv->selected;
    int count = static_cast<int>(pages.size());
    if (sel < 0)      sel = 0;
    if (sel >= count) sel = count - 1;
    return static_cast<uint32_t>(sel);
  }

  static void NEUI_ABI tabs_set_selected(neui_session_t session, neui_widget_t widget,
                                         uint32_t index)
  {
    auto* tv = tabview_from(session, widget, nullptr);
    if (!tv) return;
    // Clamp huge / sentinel indices (e.g. NEUI_ITEM_NONE) to a representable
    // int so the cast doesn't wrap negative - per the documented "clamped to
    // [0, count)", an out-of-range index selects the LAST tab, not the first.
    int ni = index > 0x7fffffffu ? 0x7fffffff : static_cast<int>(index);
    tv->select_tab(ni);
  }

  static neui_widget_t NEUI_ABI tabs_get_page(neui_session_t session,
                                              neui_widget_t widget, uint32_t index)
  {
    Session* s = nullptr;
    auto* tv = tabview_from(session, widget, &s);
    if (!tv || !s) return { UINT32_MAX };
    std::vector<uint32_t> pages; tv->collect_pages(pages);
    if (index >= pages.size()) return { UINT32_MAX };
    return IndexToWidget(s->_session_id, pages[index]);
  }

  static uint32_t NEUI_ABI tabs_get_index(neui_session_t session, neui_widget_t widget,
                                          neui_widget_t page)
  {
    auto* tv = tabview_from(session, widget, nullptr);
    if (!tv) return NEUI_ITEM_NONE;
    uint32_t page_idx = WidgetToIndex(page);
    std::vector<uint32_t> pages; tv->collect_pages(pages);
    for (uint32_t i = 0; i < pages.size(); ++i)
      if (pages[i] == page_idx) return i;
    return NEUI_ITEM_NONE;
  }

  neui_tabs_api_t tabs_api = {
    NEUI_VERSION,
    tabs_count,
    tabs_get_selected,
    tabs_set_selected,
    tabs_get_page,
    tabs_get_index,
  };

  // ---------------------------------------------------------------------------
  // Notify API (NEUI_API_NOTIFY) - toast + message box. Host-owned chrome
  // anchored to a frame, outside the widget tree.
  // ---------------------------------------------------------------------------

  static void NEUI_ABI notify_toast(neui_session_t session,
                                     neui_widget_t parent_window,
                                     const char* text)
  {
    auto* s = get_session_for_widget(session, parent_window);
    if (!s) return;
    uint32_t idx = WidgetToIndex(parent_window);
    if (!s->_widgets.exists(idx)) return;
    if (!s->_widgets[idx].is_frame()) return;
    s->toast_show(idx, text);
  }

  static int NEUI_ABI notify_message_box(neui_session_t session,
                                          neui_widget_t parent_window,
                                          const char* text, const char* caption,
                                          uint32_t flags)
  {
    auto* s = get_session_for_widget(session, parent_window);
    if (!s) return 0;
    uint32_t idx = WidgetToIndex(parent_window);
    if (!s->_widgets.exists(idx)) return 0;
    auto& wd = s->_widgets[idx];
    if (!wd.is_frame() || !wd.native_handle) return 0;
    return platform_message_box(wd.native_handle, text ? text : "",
                                 caption, flags);
  }

  neui_notify_api_t notify_api = {
    NEUI_VERSION,
    notify_toast,
    notify_message_box,
  };

  // ---------------------------------------------------------------------------
  // Asset API (NEUI_API_ASSETS) - session-scoped media handles backed by
  // the AssetManager's handle table. Handles encode the session id in the
  // upper 16 bits like neui_widget_t; cross-session handles are dropped.

  static neui_asset_t pack_asset(uint32_t session_id, uint32_t slot)
  {
    return { ((session_id & 0xffff) << 16) | (slot & 0xffff) };
  }

  static neui_asset_t NEUI_ABI as_create_bitmap(neui_session_t session,
                                                  uint32_t width_px,
                                                  uint32_t height_px,
                                                  const uint8_t* bgra,
                                                  float scale)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_bitmap(width_px, height_px,
                                                       bgra, scale);
    if (slot == 0) return asset_none;
    return pack_asset(s->_session_id, slot);
  }

  // Best-guess @Nx scale for a file load: the highest DPI of any frame in
  // this session. Falls back to 1.0 if no frames are realized yet.
  static float best_asset_scale(Session* s)
  {
    float scale = 1.0f;
    if (s && s->_backend && s->_backend->get_scale_factor) {
      uint32_t child = s->_widgets.child(0);
      while (child != 0) {
        if (s->_widgets.exists(child)) {
          auto& wd = s->_widgets[child];
          if (wd.render_ctx) {
            float ws = s->_backend->get_scale_factor(wd.render_ctx);
            if (ws > scale) scale = ws;
          }
        }
        child = s->_widgets.next(child);
      }
    }
    return scale;
  }

  static neui_asset_t NEUI_ABI as_create_from_file(neui_session_t session,
                                                     const char* path_utf8)
  {
    auto* s = get_session(session);
    if (!s || !path_utf8) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_from_file(path_utf8,
                                                         best_asset_scale(s));
    if (slot == 0) return asset_none;
    return pack_asset(s->_session_id, slot);
  }

  static void NEUI_ABI as_destroy(neui_session_t session, neui_asset_t asset)
  {
    auto* s = get_session(session);
    if (!s) return;
    if (asset.id == asset_none.id) return;
    if (((asset.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return;
    s->_asset_manager.release_slot(asset.id & 0xffff, s->_backend);
  }

  static bool NEUI_ABI as_get_size(neui_session_t session, neui_asset_t asset,
                                     float* out_w, float* out_h)
  {
    auto* s = get_session(session);
    if (!s) return false;
    if (asset.id == asset_none.id) return false;
    if (((asset.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return false;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!e) return false;
    if (e->kind == NEUI_ASSET_KIND_COMPONENT) {
      if (out_w) *out_w = e->comp_w;
      if (out_h) *out_h = e->comp_h;
      return true;
    }
    if (e->scale <= 0.0f) return false;
    if (out_w) *out_w = static_cast<float>(e->width_px)  / e->scale;
    if (out_h) *out_h = static_cast<float>(e->height_px) / e->scale;
    return true;
  }

  static neui_asset_kind_t NEUI_ABI as_get_kind(neui_session_t session,
                                                  neui_asset_t asset)
  {
    auto* s = get_session(session);
    if (!s) return NEUI_ASSET_KIND_NONE;
    if (asset.id == asset_none.id) return NEUI_ASSET_KIND_NONE;
    if (((asset.id >> 16) & 0xffff) != (s->_session_id & 0xffff))
      return NEUI_ASSET_KIND_NONE;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    return e ? e->kind : NEUI_ASSET_KIND_NONE;
  }

  static neui_asset_t NEUI_ABI as_create_compound(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_compound();
    if (slot == 0) return asset_none;
    return pack_asset(s->_session_id, slot);
  }

  static neui_asset_t NEUI_ABI as_create_behavior(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_behavior();
    if (slot == 0) return asset_none;
    return pack_asset(s->_session_id, slot);
  }

  // Defined in host.cpp - same thunk WIDGET_PAINT installs into the
  // painter so nested draw_asset works from inside a surface paint.
  void NEUI_ABI xpl_painter_draw_asset_thunk(
      void* host_token,
      neui_render_backend_t* backend,
      neui_render_ctx_t ctx,
      neui_asset_t asset,
      float x, float y, float w, float h,
      uint32_t frame,
      uint32_t tint);

  static neui_asset_t NEUI_ABI as_create_surface(neui_session_t session,
                                                   float width_logical,
                                                   float height_logical,
                                                   float scale)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    if (width_logical <= 0.0f || height_logical <= 0.0f) return asset_none;
    if (scale <= 0.0f) scale = 1.0f;
    uint32_t w_px = static_cast<uint32_t>(width_logical  * scale + 0.5f);
    uint32_t h_px = static_cast<uint32_t>(height_logical * scale + 0.5f);
    uint32_t slot = s->_asset_manager.allocate_surface(w_px, h_px, scale, s->_backend);
    if (slot == 0) return asset_none;
    return pack_asset(s->_session_id, slot);
  }

  static void NEUI_ABI as_paint_surface(neui_session_t        session,
                                          neui_asset_t          surface,
                                          uint32_t              clear_argb,
                                          neui_surface_paint_fn fn,
                                          void*                 user)
  {
    auto* s = get_session(session);
    if (!s) return;
    if (surface.id == asset_none.id) return;
    if (((surface.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return;
    s->_asset_manager.paint_surface(surface.id & 0xffff, clear_argb, fn, user,
                                     s->_backend,
                                     /*host_token*/ s,
                                     &xpl_painter_draw_asset_thunk);
  }

  static neui_asset_t NEUI_ABI as_create_font(neui_session_t session,
                                               const uint8_t* data,
                                               uint32_t       len)
  {
    auto* s = get_session(session);
    if (!s || !data || len == 0) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_font(data, len, s->_backend);
    if (slot == 0) return asset_none;
    return pack_asset(s->_session_id, slot);
  }

  static neui_asset_t NEUI_ABI as_create_font_from_file(neui_session_t session,
                                                        const char* path_utf8)
  {
    auto* s = get_session(session);
    if (!s || !path_utf8) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_font_from_file(path_utf8,
                                                              s->_backend);
    if (slot == 0) return asset_none;
    return pack_asset(s->_session_id, slot);
  }

  static uint32_t NEUI_ABI as_get_font_family(neui_session_t session,
                                              neui_asset_t font,
                                              char* out_buf, uint32_t cap)
  {
    auto* s = get_session(session);
    if (!s || font.id == asset_none.id) return 0;
    if (((font.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return 0;
    return s->_asset_manager.get_font_family(font.id & 0xffff, out_buf, cap);
  }

  // Asset / compound / behavior api tables (asset_api is defined just below;
  // compound_api / behavior_api later in this TU). Forward-declared here so the
  // component thunks can hand all three to build_component.
  extern neui_asset_api_t    asset_api;
  extern neui_compound_api_t compound_api;
  extern neui_behavior_api_t behavior_api;

  // Release a partially / fully built component's sub-assets (used on the
  // failure paths and shared by both create_component thunks).
  static void release_built_component(neui_session_t session,
                                      neui_detail::BuiltComponent& built)
  {
    if (built.compound.id != asset_none.id) as_destroy(session, built.compound);
    if (built.behavior.id != asset_none.id) as_destroy(session, built.behavior);
    for (auto a : built.owned_assets) as_destroy(session, a);
  }

  static neui_asset_t NEUI_ABI as_create_component_from_string(
      neui_session_t session, const char* json, uint32_t len,
      const neui_component_env_t* env)
  {
    auto* s = get_session(session);
    if (!s || !json) return asset_none;
    neui_detail::ComponentApis apis;
    apis.asset    = &asset_api;
    apis.compound = &compound_api;
    apis.behavior = &behavior_api;
    neui_detail::BuiltComponent built =
        neui_detail::build_component(session, json, len, env, apis);
    if (!built.ok) { release_built_component(session, built); return asset_none; }
    uint32_t slot = s->_asset_manager.allocate_component(built);
    if (slot == 0) { release_built_component(session, built); return asset_none; }
    return pack_asset(s->_session_id, slot);
  }

  static neui_asset_t NEUI_ABI as_create_component_from_file(
      neui_session_t session, const char* path_utf8,
      const neui_component_env_t* env)
  {
    auto* s = get_session(session);
    if (!s || !path_utf8) return asset_none;

    // Read the whole file (std::ifstream avoids the fopen /W4 deprecation).
    std::ifstream in(path_utf8, std::ios::binary);
    if (!in) return asset_none;
    std::string data((std::istreambuf_iterator<char>(in)),
                     std::istreambuf_iterator<char>());

    // Default env.base_dir to the file's directory so relative asset paths
    // resolve next to the .json. A caller-supplied env wins.
    neui_component_env_t local{};
    const neui_component_env_t* use_env = env;
    if (!env || !env->base_dir) {
      std::string p = path_utf8;
      size_t cut = p.find_last_of("/\\");
      static thread_local std::string base_keep;
      base_keep = (cut == std::string::npos) ? std::string() : p.substr(0, cut);
      if (env) local = *env;
      local.base_dir = base_keep.c_str();
      use_env = &local;
    }
    return as_create_component_from_string(session, data.c_str(),
                                           static_cast<uint32_t>(data.size()),
                                           use_env);
  }

  static uint32_t NEUI_ABI as_component_param_count(neui_session_t session,
                                                    neui_asset_t component)
  {
    auto* s = get_session(session);
    if (!s || component.id == asset_none.id) return 0;
    if (((component.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return 0;
    auto* e = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPONENT) return 0;
    return static_cast<uint32_t>(e->comp_params.size());
  }

  static bool NEUI_ABI as_component_param_at(neui_session_t session,
                                             neui_asset_t component,
                                             uint32_t index,
                                             neui_component_param_t* out)
  {
    auto* s = get_session(session);
    if (!s || !out || component.id == asset_none.id) return false;
    if (((component.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return false;
    auto* e = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPONENT) return false;
    if (index >= e->comp_params.size()) return false;
    const auto& p = e->comp_params[index];
    out->key   = p.key.c_str();
    out->label = p.label.c_str();
    out->min   = p.min;
    out->max   = p.max;
    out->def   = p.def;
    return true;
  }

  static uint32_t NEUI_ABI as_serialize_component(neui_session_t session,
                                                  neui_asset_t component,
                                                  char* out_buf, uint32_t cap,
                                                  int indent)
  {
    auto* s = get_session(session);
    if (!s || component.id == asset_none.id) return 0;
    if (((component.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return 0;
    auto* e = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPONENT) return 0;

    neui_detail::ComponentSerializeInput in;
    in.name               = &e->comp_name;
    in.width              = e->comp_w;
    in.height             = e->comp_h;
    in.params             = &e->comp_params;
    in.asset_names        = &e->comp_asset_names;
    in.asset_handle_names = &e->comp_asset_handle_names;
    in.asset_frame_layouts = &e->comp_asset_frame_layouts;
    auto* ce = s->_asset_manager.get_slot(e->comp_compound.id & 0xffff);
    auto* be = s->_asset_manager.get_slot(e->comp_behavior.id & 0xffff);
    in.compound = (ce && ce->compound) ? ce->compound.get() : nullptr;
    in.behavior = (be && be->behavior) ? be->behavior.get() : nullptr;

    std::string json = neui_detail::serialize_component(in, indent);
    uint32_t full = static_cast<uint32_t>(json.size());
    if (out_buf && cap > 0) {
      uint32_t n = (full > cap - 1) ? cap - 1 : full;
      if (n) std::memcpy(out_buf, json.data(), n);
      out_buf[n] = '\0';
    }
    return full;
  }

  // Attach a COMPONENT's compound + behavior to a CUSTOMDRAW widget and stamp
  // its default attrs (only keys the widget doesn't already carry, so client
  // pre-sets win). Shared by w_set_asset (COMPONENT route) + create_from_component.
  static void attach_component(Session* s, uint32_t idx,
                               neui_detail::AssetEntry* ce)
  {
    if (!s || !ce || ce->kind != NEUI_ASSET_KIND_COMPONENT) return;
    if (!s->_widgets.exists(idx)) return;
    auto& wd = s->_widgets[idx];
    auto* cd = dynamic_cast<CustomDrawWidget*>(&wd);
    if (!cd) return;
    cd->compound_asset = ce->comp_compound;
    cd->behavior_asset = ce->comp_behavior;
    auto& bag = neui_detail::ensure_attrs(wd.attrs);
    for (const auto& d : ce->comp_defaults) {
      if (bag.has(d.key)) continue;  // client pre-set wins
      switch (d.type) {
        case neui_detail::ComponentDefaultAttr::INT:    bag.set_int(d.key, d.ival); break;
        case neui_detail::ComponentDefaultAttr::FLOAT:  bag.set_float(d.key, d.fval); break;
        case neui_detail::ComponentDefaultAttr::STRING: bag.set_string(d.key, d.sval.c_str()); break;
      }
    }
  }

  static neui_widget_t NEUI_ABI w_create_from_component(
      neui_session_t session, neui_widget_t parent, neui_asset_t component,
      int x, int y, int width, int height)
  {
    auto* s = get_session_for_widget(session, parent);
    if (!s || component.id == asset_none.id) return widget_none;
    if (((component.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return widget_none;
    auto* ce = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!ce || ce->kind != NEUI_ASSET_KIND_COMPONENT) return widget_none;

    if (width  <= 0) width  = static_cast<int>(ce->comp_w + 0.5f);
    if (height <= 0) height = static_cast<int>(ce->comp_h + 0.5f);

    neui_widget_t w = w_create(session, parent, NEUI_W_CUSTOMDRAW,
                               x, y, width, height, nullptr);
    if (w.id == widget_none.id) return widget_none;

    // Re-resolve the entry (w_create does not touch the asset table, but keep
    // the lookup local so a future change can't leave a stale pointer).
    ce = s->_asset_manager.get_slot(component.id & 0xffff);
    attach_component(s, WidgetToIndex(w), ce);
    if (void* frame = s->find_parent_native_handle(WidgetToIndex(w)))
      platform_invalidate(frame);
    return w;
  }

  static bool NEUI_ABI as_set_frame_layout(neui_session_t session,
                                           neui_asset_t asset,
                                           uint32_t cols, uint32_t rows,
                                           uint32_t gutter_px)
  {
    auto* s = get_session(session);
    if (!s || asset.id == asset_none.id) return false;
    if (((asset.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return false;
    return s->_asset_manager.set_frame_layout(asset.id & 0xffff,
                                              cols, rows, gutter_px);
  }

  static neui_asset_t NEUI_ABI as_create_filmstrip_from_file(
      neui_session_t session, const char* path_utf8,
      uint32_t frame_count, neui_filmstrip_orientation_t orientation)
  {
    auto* s = get_session(session);
    if (!s || !path_utf8) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_filmstrip_from_file(
        path_utf8, best_asset_scale(s), frame_count,
        orientation == NEUI_FILMSTRIP_HORIZONTAL, s->_backend);
    if (slot == 0) return asset_none;
    return pack_asset(s->_session_id, slot);
  }

  static uint32_t NEUI_ABI as_get_frame_count(neui_session_t session,
                                              neui_asset_t asset)
  {
    auto* s = get_session(session);
    if (!s || asset.id == asset_none.id) return 0;
    if (((asset.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return 0;
    return s->_asset_manager.frame_count(asset.id & 0xffff);
  }

  neui_asset_api_t asset_api = {
    NEUI_VERSION,
    as_create_bitmap,
    as_create_from_file,
    as_destroy,
    as_get_size,
    as_get_kind,
    as_create_compound,
    as_create_behavior,
    as_create_surface,
    as_paint_surface,
    as_create_font,
    as_create_font_from_file,
    as_get_font_family,
    as_create_component_from_string,
    as_create_component_from_file,
    as_component_param_count,
    as_component_param_at,
    as_serialize_component,
    as_set_frame_layout,
    as_create_filmstrip_from_file,
    as_get_frame_count,
  };

  // ===========================================================================
  // Compound API (NEUI_API_COMPOUND)
  //
  // Operates on neui_asset_t handles created via as_create_compound. Each
  // thunk validates the session match in the handle's upper 16 bits, looks
  // up the AssetEntry, confirms it's a compound, then dispatches to the
  // shared mutator helpers in hosts/shared/compound.h.
  //
  // Layer-id encoding: (asset_slot << 16) | layer_slot - the upper half
  // identifies the owning compound asset so we can validate that a layer
  // handle from one compound isn't accidentally applied to another.
  //
  // Mutations invalidate every widget whose attached asset matches. The
  // walk is O(N_widgets) per mutation but mutations are typically
  // setup-time; the steady-state cost is zero.

  static neui_detail::CompoundLayer*
  resolve_layer(neui_session_t session, neui_asset_t asset,
                 neui_compound_layer_t layer, Session*& out_session)
  {
    out_session = nullptr;
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return nullptr;
    uint32_t asset_slot = asset.id & 0xffff;
    if (neui_detail::compound_layer_asset_slot(layer) != asset_slot) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset_slot);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPOUND || !e->compound) return nullptr;
    out_session = s;
    return neui_detail::compound_get_layer(*e->compound,
                                            neui_detail::compound_layer_slot(layer));
  }

  static neui_detail::CompoundAsset*
  resolve_compound(neui_session_t session, neui_asset_t asset, Session*& out_session)
  {
    out_session = nullptr;
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPOUND || !e->compound) return nullptr;
    out_session = s;
    return e->compound.get();
  }

  // Walk the session's widgets and invalidate any whose attached asset is
  // this compound. Used after every compound mutation.
  static void invalidate_widgets_using(Session* s, uint32_t asset_slot);

  static neui_compound_layer_t NEUI_ABI co_add_layer(neui_session_t session,
                                                       neui_asset_t asset,
                                                       neui_compound_layer_kind_t kind,
                                                       int z)
  {
    Session* s = nullptr;
    auto* ca = resolve_compound(session, asset, s);
    if (!ca) return compound_layer_none;
    uint32_t asset_slot = asset.id & 0xffff;
    uint32_t slot = neui_detail::compound_add_layer(*ca, kind, z);
    invalidate_widgets_using(s, asset_slot);
    return neui_detail::pack_compound_layer(asset_slot, slot);
  }

  static void NEUI_ABI co_remove_layer(neui_session_t session, neui_asset_t asset,
                                         neui_compound_layer_t layer)
  {
    Session* s = nullptr;
    auto* ca = resolve_compound(session, asset, s);
    if (!ca) return;
    if (neui_detail::compound_layer_asset_slot(layer) != (asset.id & 0xffff)) return;
    neui_detail::compound_remove_layer(*ca, neui_detail::compound_layer_slot(layer));
    invalidate_widgets_using(s, asset.id & 0xffff);
  }

  static void NEUI_ABI co_clear(neui_session_t session, neui_asset_t asset)
  {
    Session* s = nullptr;
    auto* ca = resolve_compound(session, asset, s);
    if (!ca) return;
    neui_detail::compound_clear(*ca);
    invalidate_widgets_using(s, asset.id & 0xffff);
  }

  static void NEUI_ABI co_set_z(neui_session_t session, neui_asset_t asset,
                                  neui_compound_layer_t layer, int z)
  {
    Session* s = nullptr;
    auto* L = resolve_layer(session, asset, layer, s);
    if (!L) return;
    L->z = z;
    invalidate_widgets_using(s, asset.id & 0xffff);
  }

  static void NEUI_ABI co_set_anchor(neui_session_t session, neui_asset_t asset,
                                       neui_compound_layer_t layer,
                                       neui_anchor_t parent_anchor,
                                       neui_anchor_t self_anchor)
  {
    Session* s = nullptr;
    auto* L = resolve_layer(session, asset, layer, s);
    if (!L) return;
    L->parent_anchor = parent_anchor;
    L->self_anchor   = self_anchor;
    invalidate_widgets_using(s, asset.id & 0xffff);
  }

  static void NEUI_ABI co_set_int(neui_session_t session, neui_asset_t asset,
                                    neui_compound_layer_t layer,
                                    const char* prop, int value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_int(*L, prop, value);
    invalidate_widgets_using(s, asset.id & 0xffff);
  }

  static void NEUI_ABI co_set_float(neui_session_t session, neui_asset_t asset,
                                      neui_compound_layer_t layer,
                                      const char* prop, float value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_float(*L, prop, value);
    invalidate_widgets_using(s, asset.id & 0xffff);
  }

  static void NEUI_ABI co_set_string(neui_session_t session, neui_asset_t asset,
                                       neui_compound_layer_t layer,
                                       const char* prop, const char* value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_string(*L, prop, value);
    invalidate_widgets_using(s, asset.id & 0xffff);
  }

  static void NEUI_ABI co_set_asset(neui_session_t session, neui_asset_t asset,
                                      neui_compound_layer_t layer,
                                      const char* prop, neui_asset_t value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_asset(*L, prop, value);
    invalidate_widgets_using(s, asset.id & 0xffff);
  }

  static void NEUI_ABI co_bind(neui_session_t session, neui_asset_t asset,
                                 neui_compound_layer_t layer,
                                 const char* prop, const char* attr_key,
                                 float scale, float offset)
  {
    Session* s = nullptr;
    auto* L = resolve_layer(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_bind(*L, prop, attr_key, scale, offset);
    invalidate_widgets_using(s, asset.id & 0xffff);
  }

  static void NEUI_ABI co_bind_asset(neui_session_t session, neui_asset_t asset,
                                       neui_compound_layer_t layer,
                                       const char* prop, const char* attr_key)
  {
    Session* s = nullptr;
    auto* L = resolve_layer(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_bind_asset(*L, prop, attr_key);
    invalidate_widgets_using(s, asset.id & 0xffff);
  }

  static void NEUI_ABI co_unbind(neui_session_t session, neui_asset_t asset,
                                   neui_compound_layer_t layer, const char* prop)
  {
    Session* s = nullptr;
    auto* L = resolve_layer(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_unbind(*L, prop);
    invalidate_widgets_using(s, asset.id & 0xffff);
  }

  static void NEUI_ABI co_set_path(neui_session_t session, neui_asset_t asset,
                                     neui_compound_layer_t layer,
                                     const neui_path_cmd_t* cmds,
                                     uint32_t count)
  {
    Session* s = nullptr;
    auto* L = resolve_layer(session, asset, layer, s);
    if (!L) return;
    neui_detail::apply_set_path(*L, cmds, count);
    invalidate_widgets_using(s, asset.id & 0xffff);
  }

  neui_compound_api_t compound_api = {
    NEUI_VERSION,
    co_add_layer,
    co_remove_layer,
    co_clear,
    co_set_z,
    co_set_anchor,
    co_set_int,
    co_set_float,
    co_set_string,
    co_set_asset,
    co_bind,
    co_bind_asset,
    co_unbind,
    co_set_path,
  };

  // Recursive walk: collect frame native handles that own at least one
  // CustomDrawWidget whose compound_asset matches asset_id, then
  // platform_invalidate each. asset_id is the full 32-bit handle id.
  static void invalidate_walk_xpl(Session* s, uint32_t parent_idx,
                                    uint32_t asset_id,
                                    std::vector<void*>& already)
  {
    uint32_t child = s->_widgets.child(parent_idx);
    while (child != 0) {
      if (s->_widgets.exists(child)) {
        auto& wd = s->_widgets[child];
        if (auto* cd = dynamic_cast<CustomDrawWidget*>(&wd)) {
          if (cd->compound_asset.id == asset_id) {
            if (void* frame = s->find_parent_native_handle(child)) {
              if (std::find(already.begin(), already.end(), frame) == already.end()) {
                already.push_back(frame);
                platform_invalidate(frame);
              }
            }
          }
        }
        invalidate_walk_xpl(s, child, asset_id, already);
      }
      child = s->_widgets.next(child);
    }
  }

  static void invalidate_widgets_using(Session* s, uint32_t asset_slot)
  {
    if (!s) return;
    uint32_t asset_id = ((s->_session_id & 0xffff) << 16) | (asset_slot & 0xffff);
    std::vector<void*> already;
    invalidate_walk_xpl(s, 0, asset_id, already);
  }

  // ===========================================================================
  // Behavior API (NEUI_API_BEHAVIOR)

  static neui_detail::BehaviorAsset*
  resolve_behavior(neui_session_t session, neui_asset_t asset, Session*& out_session)
  {
    out_session = nullptr;
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_BEHAVIOR || !e->behavior) return nullptr;
    out_session = s;
    return e->behavior.get();
  }

  static neui_detail::BehaviorHandler*
  resolve_behavior_handler(neui_session_t session, neui_asset_t asset,
                            neui_behavior_handler_t handler, Session*& out_session)
  {
    out_session = nullptr;
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->_session_id & 0xffff)) return nullptr;
    uint32_t asset_slot = asset.id & 0xffff;
    if (neui_detail::behavior_handler_asset_slot(handler) != asset_slot) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset_slot);
    if (!e || e->kind != NEUI_ASSET_KIND_BEHAVIOR || !e->behavior) return nullptr;
    out_session = s;
    return neui_detail::behavior_get_handler(*e->behavior,
                                              neui_detail::behavior_handler_slot(handler));
  }

  // Behavior mutations don't change paint output, so we skip the
  // invalidate-widgets walk that compound mutations need. Visual updates
  // happen only when the behavior actually writes an attr at run time,
  // and that already triggers invalidate via the per-write callback.
  static neui_behavior_handler_t NEUI_ABI be_add_handler(neui_session_t session,
                                                          neui_asset_t asset,
                                                          neui_behavior_kind_t kind)
  {
    Session* s = nullptr;
    auto* ba = resolve_behavior(session, asset, s);
    if (!ba) return behavior_handler_none;
    uint32_t asset_slot = asset.id & 0xffff;
    uint32_t slot = neui_detail::behavior_add_handler(*ba, kind);
    return neui_detail::pack_behavior_handler(asset_slot, slot);
  }

  static void NEUI_ABI be_remove_handler(neui_session_t session, neui_asset_t asset,
                                          neui_behavior_handler_t handler)
  {
    Session* s = nullptr;
    auto* ba = resolve_behavior(session, asset, s);
    if (!ba) return;
    if (neui_detail::behavior_handler_asset_slot(handler) != (asset.id & 0xffff)) return;
    neui_detail::behavior_remove_handler(*ba, neui_detail::behavior_handler_slot(handler));
  }

  static void NEUI_ABI be_clear(neui_session_t session, neui_asset_t asset)
  {
    Session* s = nullptr;
    auto* ba = resolve_behavior(session, asset, s);
    if (!ba) return;
    neui_detail::behavior_clear(*ba);
  }

  static void NEUI_ABI be_set_int(neui_session_t session, neui_asset_t asset,
                                    neui_behavior_handler_t handler,
                                    const char* prop, int value)
  {
    Session* s = nullptr;
    auto* H = resolve_behavior_handler(session, asset, handler, s);
    if (!H || !prop) return;
    neui_detail::apply_behavior_set_int(*H, prop, value);
  }

  static void NEUI_ABI be_set_float(neui_session_t session, neui_asset_t asset,
                                      neui_behavior_handler_t handler,
                                      const char* prop, float value)
  {
    Session* s = nullptr;
    auto* H = resolve_behavior_handler(session, asset, handler, s);
    if (!H || !prop) return;
    neui_detail::apply_behavior_set_float(*H, prop, value);
  }

  static void NEUI_ABI be_set_string(neui_session_t session, neui_asset_t asset,
                                       neui_behavior_handler_t handler,
                                       const char* prop, const char* value)
  {
    Session* s = nullptr;
    auto* H = resolve_behavior_handler(session, asset, handler, s);
    if (!H || !prop) return;
    neui_detail::apply_behavior_set_string(*H, prop, value);
  }

  neui_behavior_api_t behavior_api = {
    NEUI_VERSION,
    be_add_handler,
    be_remove_handler,
    be_clear,
    be_set_int,
    be_set_float,
    be_set_string,
  };

  // -------------------------------------------------------------------------
  // Grid API (NEUI_API_GRID) - thin wrapper over GridWidget::model.

  static GridWidget* resolve_grid(neui_session_t session, neui_widget_t widget,
                                    Session** out_sess = nullptr)
  {
    auto* s = get_session_for_widget(session, widget);
    if (out_sess) *out_sess = s;
    if (!s) return nullptr;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return nullptr;
    return dynamic_cast<GridWidget*>(&s->_widgets[idx]);
  }

  static void grid_invalidate(Session* s, GridWidget* g)
  {
    if (!s || !g) return;
    void* frame = s->find_parent_native_handle(g->index);
    if (frame) platform_invalidate(frame);
  }

  static int NEUI_ABI gr_add_column(neui_session_t session, neui_widget_t widget,
                                      const char* header, int width_logical)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return -1;
    neui_detail::GridColumn c;
    c.header = header ? header : "";
    c.width  = (width_logical > 0) ? width_logical : neui_detail::GRID_DEFAULT_NEW_COLUMN_W;
    g->model.columns.push_back(std::move(c));
    neui_detail::grid_resize_rows_to_columns(g->model,
                                                (int)g->model.columns.size());
    grid_invalidate(s, g);
    return (int)g->model.columns.size() - 1;
  }

  static int NEUI_ABI gr_get_column_count(neui_session_t session, neui_widget_t widget)
  {
    auto* g = resolve_grid(session, widget);
    return g ? (int)g->model.columns.size() : 0;
  }

  static void NEUI_ABI gr_set_column_width(neui_session_t session, neui_widget_t widget,
                                             int col, int width_logical)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g || col < 0 || col >= (int)g->model.columns.size()) return;
    auto cfg = neui_detail::grid_read_config(g->attrs.get());
    int min_w = neui_detail::grid_column_min_width(g->model, col, cfg.col_min_w_def);
    if (width_logical < min_w) width_logical = min_w;
    g->model.columns[(size_t)col].width = width_logical;
    grid_invalidate(s, g);
  }

  static int NEUI_ABI gr_get_column_width(neui_session_t session, neui_widget_t widget, int col)
  {
    auto* g = resolve_grid(session, widget);
    if (!g || col < 0 || col >= (int)g->model.columns.size()) return 0;
    return g->model.columns[(size_t)col].width;
  }

  static void NEUI_ABI gr_set_column_min_width(neui_session_t session, neui_widget_t widget,
                                                  int col, int min_w)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g || col < 0 || col >= (int)g->model.columns.size()) return;
    g->model.columns[(size_t)col].min_width = min_w;
    if (g->model.columns[(size_t)col].width < min_w)
      g->model.columns[(size_t)col].width = min_w;
    grid_invalidate(s, g);
  }

  static void NEUI_ABI gr_set_column_align(neui_session_t session, neui_widget_t widget,
                                             int col, const char* align)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g || col < 0 || col >= (int)g->model.columns.size()) return;
    g->model.columns[(size_t)col].align = neui_detail::grid_parse_align(align);
    grid_invalidate(s, g);
  }

  static void NEUI_ABI gr_set_column_header(neui_session_t session, neui_widget_t widget,
                                              int col, const char* text)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g || col < 0 || col >= (int)g->model.columns.size()) return;
    g->model.columns[(size_t)col].header = text ? text : "";
    grid_invalidate(s, g);
  }

  static int NEUI_ABI gr_get_column_header(neui_session_t session, neui_widget_t widget,
                                             int col, char* buf, int buflen)
  {
    auto* g = resolve_grid(session, widget);
    if (!g || col < 0 || col >= (int)g->model.columns.size()) return -1;
    const std::string& h = g->model.columns[(size_t)col].header;
    int need = (int)h.size() + 1;
    if (buf && buflen > 0) {
      int copy = (need < buflen) ? need : buflen;
      memcpy(buf, h.c_str(), (size_t)copy);
      buf[copy - 1] = 0;
    }
    return need;
  }

  static void NEUI_ABI gr_remove_column(neui_session_t session, neui_widget_t widget, int col)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g || col < 0 || col >= (int)g->model.columns.size()) return;
    g->model.columns.erase(g->model.columns.begin() + col);
    // Drop the matching cell from every row.
    for (auto& r : g->model.rows) {
      if (col < (int)r.cells.size()) r.cells.erase(r.cells.begin() + col);
    }
    // Drop cell overrides on the removed column; shift higher-col entries left.
    std::unordered_map<uint64_t, neui_detail::GridCellOverride> remap;
    for (auto& kv : g->model.cell_overrides) {
      int r = (int)(kv.first >> 32);
      int c = (int)(kv.first & 0xFFFFFFFF);
      if (c == col) continue;
      int nc = (c > col) ? c - 1 : c;
      remap[neui_detail::grid_cell_key(r, nc)] = kv.second;
    }
    g->model.cell_overrides = std::move(remap);
    if (g->model.selected_col >= (int)g->model.columns.size())
      g->model.selected_col = (int)g->model.columns.size() - 1;
    neui_detail::grid_sort_on_column_removed(g->model, col);
    grid_invalidate(s, g);
  }

  static void NEUI_ABI gr_clear_columns(neui_session_t session, neui_widget_t widget)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return;
    g->model.columns.clear();
    g->model.rows.clear();
    g->model.cell_overrides.clear();
    g->model.selected_row = -1;
    g->model.selected_col = -1;
    g->model.scroll_offset_x = 0;
    g->model.scroll_offset_y = 0;
    g->model.sort_stack.clear();
    g->model.display_order.clear();
    g->model.logical_to_visual.clear();
    g->model.sort_dirty = false;
    grid_invalidate(s, g);
  }

  static int NEUI_ABI gr_add_row(neui_session_t session, neui_widget_t widget,
                                   const char* const* values_utf8)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return -1;
    neui_detail::GridRow row;
    row.cells.resize(g->model.columns.size());
    if (values_utf8) {
      for (size_t i = 0; i < g->model.columns.size() && values_utf8[i]; ++i)
        row.cells[i] = values_utf8[i];
    }
    g->model.rows.push_back(std::move(row));
    g->model.sort_dirty = true;
    grid_invalidate(s, g);
    return (int)g->model.rows.size() - 1;
  }

  static int NEUI_ABI gr_get_row_count(neui_session_t session, neui_widget_t widget)
  {
    auto* g = resolve_grid(session, widget);
    return g ? (int)g->model.rows.size() : 0;
  }

  static void NEUI_ABI gr_remove_row(neui_session_t session, neui_widget_t widget, int row)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g || row < 0 || row >= (int)g->model.rows.size()) return;
    g->model.rows.erase(g->model.rows.begin() + row);
    // Drop overrides on the removed row; shift higher rows down.
    std::unordered_map<uint64_t, neui_detail::GridCellOverride> remap;
    for (auto& kv : g->model.cell_overrides) {
      int r = (int)(kv.first >> 32);
      int c = (int)(kv.first & 0xFFFFFFFF);
      if (r == row) continue;
      int nr = (r > row) ? r - 1 : r;
      remap[neui_detail::grid_cell_key(nr, c)] = kv.second;
    }
    g->model.cell_overrides = std::move(remap);
    if (g->model.selected_row >= (int)g->model.rows.size())
      g->model.selected_row = (int)g->model.rows.size() - 1;
    g->model.sort_dirty = true;
    grid_invalidate(s, g);
  }

  static void NEUI_ABI gr_clear_rows(neui_session_t session, neui_widget_t widget)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return;
    g->model.rows.clear();
    g->model.cell_overrides.clear();
    g->model.selected_row = -1;
    g->model.scroll_offset_y = 0;
    g->model.display_order.clear();
    g->model.logical_to_visual.clear();
    g->model.sort_dirty = false;
    grid_invalidate(s, g);
  }

  static void NEUI_ABI gr_set_cell_text(neui_session_t session, neui_widget_t widget,
                                          int row, int col, const char* utf8)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g || row < 0 || row >= (int)g->model.rows.size()) return;
    if (col < 0 || col >= (int)g->model.columns.size()) return;
    auto& r = g->model.rows[(size_t)row];
    if ((int)r.cells.size() <= col) r.cells.resize((size_t)col + 1);
    r.cells[(size_t)col] = utf8 ? utf8 : "";
    g->model.sort_dirty = true;
    grid_invalidate(s, g);
  }

  static int NEUI_ABI gr_get_cell_text(neui_session_t session, neui_widget_t widget,
                                         int row, int col, char* buf, int buflen)
  {
    auto* g = resolve_grid(session, widget);
    if (!g || row < 0 || row >= (int)g->model.rows.size()) return -1;
    if (col < 0 || col >= (int)g->model.columns.size()) return -1;
    const auto& r = g->model.rows[(size_t)row];
    static const std::string empty;
    const std::string& src = (col < (int)r.cells.size()) ? r.cells[(size_t)col] : empty;
    int need = (int)src.size() + 1;
    if (buf && buflen > 0) {
      int copy = (need < buflen) ? need : buflen;
      memcpy(buf, src.c_str(), (size_t)copy);
      buf[copy - 1] = 0;
    }
    return need;
  }

  static void NEUI_ABI gr_set_cell_color(neui_session_t session, neui_widget_t widget,
                                           int row, int col, uint32_t argb)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g || row < 0 || row >= (int)g->model.rows.size()) return;
    if (col < 0 || col >= (int)g->model.columns.size()) return;
    if (argb == 0) {
      auto* ov = neui_detail::grid_find_override(g->model, row, col);
      if (ov) {
        ov->has_color = false;
        ov->color     = 0;
        neui_detail::grid_prune_override(g->model, row, col);
      }
    } else {
      auto& ov = neui_detail::grid_ensure_override(g->model, row, col);
      ov.color     = argb;
      ov.has_color = true;
    }
    grid_invalidate(s, g);
  }

  static void NEUI_ABI gr_set_cell_enabled(neui_session_t session, neui_widget_t widget,
                                             int row, int col, bool enabled)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g || row < 0 || row >= (int)g->model.rows.size()) return;
    if (col < 0 || col >= (int)g->model.columns.size()) return;
    auto& ov = neui_detail::grid_ensure_override(g->model, row, col);
    ov.enabled     = enabled;
    ov.has_enabled = true;
    if (enabled) {
      ov.has_enabled = !enabled ? true : true;
      // When re-enabling and that's the only override, drop it.
      if (enabled && !ov.has_color) {
        ov.has_enabled = false;
        neui_detail::grid_prune_override(g->model, row, col);
      }
    }
    grid_invalidate(s, g);
  }

  static void NEUI_ABI gr_clear_cell_overrides(neui_session_t session, neui_widget_t widget,
                                                  int row, int col)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return;
    g->model.cell_overrides.erase(neui_detail::grid_cell_key(row, col));
    grid_invalidate(s, g);
  }

  static void NEUI_ABI gr_set_selected_row(neui_session_t session, neui_widget_t widget, int row)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return;
    int n = (int)g->model.rows.size();
    if (row < -1)  row = -1;
    if (row >= n)  row = n - 1;
    g->model.selected_row = row;
    auto cfg = neui_detail::grid_read_config(g->attrs.get());
    if (cfg.cell_focus && g->model.selected_col < 0 &&
        !g->model.columns.empty())
      g->model.selected_col = 0;
    if (row >= 0) {
      auto vp = neui_detail::grid_compute_viewport(g->model, g->width, g->height,
                                                     cfg.row_h, cfg.header_h);
      neui_detail::grid_ensure_row_visible(g->model, vp, cfg.row_h, row);
    }
    grid_invalidate(s, g);
  }

  static int NEUI_ABI gr_get_selected_row(neui_session_t session, neui_widget_t widget)
  {
    auto* g = resolve_grid(session, widget);
    return g ? g->model.selected_row : -1;
  }

  static void NEUI_ABI gr_set_selected_cell(neui_session_t session, neui_widget_t widget,
                                              int row, int col)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return;
    int n_rows = (int)g->model.rows.size();
    int n_cols = (int)g->model.columns.size();
    if (row < -1)       row = -1;
    if (row >= n_rows)  row = n_rows - 1;
    if (col < -1)       col = -1;
    if (col >= n_cols)  col = n_cols - 1;
    g->model.selected_row = row;
    g->model.selected_col = col;
    if (row >= 0 && col >= 0) {
      auto cfg = neui_detail::grid_read_config(g->attrs.get());
      auto vp  = neui_detail::grid_compute_viewport(g->model, g->width, g->height,
                                                      cfg.row_h, cfg.header_h);
      neui_detail::grid_ensure_cell_visible(g->model, vp, cfg.row_h, row, col);
    }
    grid_invalidate(s, g);
  }

  static void NEUI_ABI gr_get_selected_cell(neui_session_t session, neui_widget_t widget,
                                              int* out_row, int* out_col)
  {
    auto* g = resolve_grid(session, widget);
    if (out_row) *out_row = g ? g->model.selected_row : -1;
    if (out_col) {
      if (!g) { *out_col = -1; return; }
      auto cfg = neui_detail::grid_read_config(g->attrs.get());
      *out_col = cfg.cell_focus ? g->model.selected_col : -1;
    }
  }

  static void NEUI_ABI gr_ensure_row_visible(neui_session_t session, neui_widget_t widget, int row)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return;
    auto cfg = neui_detail::grid_read_config(g->attrs.get());
    auto vp  = neui_detail::grid_compute_viewport(g->model, g->width, g->height,
                                                    cfg.row_h, cfg.header_h);
    neui_detail::grid_ensure_row_visible(g->model, vp, cfg.row_h, row);
    grid_invalidate(s, g);
  }

  static void NEUI_ABI gr_ensure_cell_visible(neui_session_t session, neui_widget_t widget,
                                                int row, int col)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return;
    auto cfg = neui_detail::grid_read_config(g->attrs.get());
    auto vp  = neui_detail::grid_compute_viewport(g->model, g->width, g->height,
                                                    cfg.row_h, cfg.header_h);
    neui_detail::grid_ensure_cell_visible(g->model, vp, cfg.row_h, row, col);
    grid_invalidate(s, g);
  }

  static void NEUI_ABI gr_set_scroll_x(neui_session_t session, neui_widget_t widget, int x)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return;
    g->model.scroll_offset_x = x;
    auto cfg = neui_detail::grid_read_config(g->attrs.get());
    auto vp  = neui_detail::grid_compute_viewport(g->model, g->width, g->height,
                                                    cfg.row_h, cfg.header_h);
    neui_detail::grid_clamp_scroll(g->model, vp, cfg.row_h);
    grid_invalidate(s, g);
  }

  static int NEUI_ABI gr_get_scroll_x(neui_session_t session, neui_widget_t widget)
  {
    auto* g = resolve_grid(session, widget);
    return g ? g->model.scroll_offset_x : 0;
  }

  static int NEUI_ABI gr_hit_test(neui_session_t session, neui_widget_t widget,
                                    int lx, int ly, int* out_row, int* out_col)
  {
    auto* g = resolve_grid(session, widget);
    if (out_row) *out_row = -1;
    if (out_col) *out_col = -1;
    if (!g) return 0;
    auto cfg = neui_detail::grid_read_config(g->attrs.get());
    auto vp  = neui_detail::grid_compute_viewport(g->model, g->width, g->height,
                                                    cfg.row_h, cfg.header_h);
    neui_detail::grid_ensure_sort_clean(g->model);
    auto hit = neui_detail::grid_hit_test(g->model, vp, cfg.row_h,
                                            g->width, g->height, lx, ly);
    if (hit.region != neui_detail::GridHitRegion::Cell) return 0;
    if (out_row) *out_row = hit.row;
    if (out_col) *out_col = hit.col;
    return 1;
  }

  // -------- Sort API ----------------------------------------------------

  static void NEUI_ABI gr_set_column_sortable(neui_session_t session, neui_widget_t widget,
                                                int col, bool sortable)
  {
    auto* g = resolve_grid(session, widget);
    if (!g) return;
    if (col < 0 || col >= (int)g->model.columns.size()) return;
    g->model.columns[(size_t)col].sortable = sortable;
  }

  static void NEUI_ABI gr_set_column_sort_kind(neui_session_t session, neui_widget_t widget,
                                                 int col, neui_grid_sort_kind_t kind)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return;
    if (col < 0 || col >= (int)g->model.columns.size()) return;
    g->model.columns[(size_t)col].sort_kind = kind;
    if (neui_detail::grid_sort_stack_find(g->model, col) >= 0) {
      g->model.sort_dirty = true;
      grid_invalidate(s, g);
    }
  }

  static void NEUI_ABI gr_set_sort(neui_session_t session, neui_widget_t widget,
                                     int col, neui_grid_sort_dir_t dir)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return;
    neui_detail::grid_set_sort(g->model, col, dir);
    grid_invalidate(s, g);
  }

  static void NEUI_ABI gr_add_sort(neui_session_t session, neui_widget_t widget,
                                     int col, neui_grid_sort_dir_t dir)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return;
    neui_detail::grid_add_sort(g->model, col, dir);
    grid_invalidate(s, g);
  }

  static void NEUI_ABI gr_clear_sort(neui_session_t session, neui_widget_t widget)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return;
    neui_detail::grid_clear_sort(g->model);
    grid_invalidate(s, g);
  }

  static int NEUI_ABI gr_get_sort_count(neui_session_t session, neui_widget_t widget)
  {
    auto* g = resolve_grid(session, widget);
    return g ? (int)g->model.sort_stack.size() : 0;
  }

  static void NEUI_ABI gr_get_sort_level(neui_session_t session, neui_widget_t widget,
                                           int level, int* out_col,
                                           neui_grid_sort_dir_t* out_dir)
  {
    if (out_col) *out_col = -1;
    if (out_dir) *out_dir = NEUI_GRID_SORT_NONE;
    auto* g = resolve_grid(session, widget);
    if (!g) return;
    if (level < 0 || level >= (int)g->model.sort_stack.size()) return;
    if (out_col) *out_col = g->model.sort_stack[(size_t)level].col;
    if (out_dir) *out_dir = g->model.sort_stack[(size_t)level].dir;
  }

  static int NEUI_ABI gr_logical_to_visual_row(neui_session_t session, neui_widget_t widget,
                                                  int logical_row)
  {
    auto* g = resolve_grid(session, widget);
    if (!g) return -1;
    if (logical_row < 0 || logical_row >= (int)g->model.rows.size()) return -1;
    neui_detail::grid_ensure_sort_clean(g->model);
    return neui_detail::grid_logical_to_visual(g->model, logical_row);
  }

  static int NEUI_ABI gr_visual_to_logical_row(neui_session_t session, neui_widget_t widget,
                                                  int visual_row)
  {
    auto* g = resolve_grid(session, widget);
    if (!g) return -1;
    if (visual_row < 0 || visual_row >= (int)g->model.rows.size()) return -1;
    neui_detail::grid_ensure_sort_clean(g->model);
    return neui_detail::grid_visual_to_logical(g->model, visual_row);
  }

  // -------- Cell editing API --------------------------------------------
  // Forward declarations of the static commit / cancel / begin helpers
  // defined in host.cpp. We deliberately keep their definitions next to
  // GridWidget there, then expose them through `extern` here, so the API
  // wrappers don't duplicate the dispatch logic.

} // namespace xpl_host

namespace xpl_host {
  class GridWidget;
  bool xpl_grid_try_begin_edit_pub(GridWidget& g, int row, int col);
  bool xpl_grid_commit_edit_pub(GridWidget& g);
  void xpl_grid_cancel_edit_pub(GridWidget& g);
}

namespace xpl_host {

  static void NEUI_ABI gr_set_column_editable(neui_session_t session, neui_widget_t widget,
                                                int col, bool editable)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g || col < 0 || col >= (int)g->model.columns.size()) return;
    g->model.columns[(size_t)col].editable = editable;
    // If we just made the editing column non-editable while it's open,
    // cancel the editor so we don't leave it dangling.
    if (!editable && g->model.edit.active && g->model.edit.col == col) {
      xpl_grid_cancel_edit_pub(*g);
      grid_invalidate(s, g);
    }
  }

  static bool NEUI_ABI gr_get_column_editable(neui_session_t session, neui_widget_t widget,
                                                int col)
  {
    auto* g = resolve_grid(session, widget);
    if (!g || col < 0 || col >= (int)g->model.columns.size()) return false;
    return g->model.columns[(size_t)col].editable;
  }

  static void NEUI_ABI gr_begin_cell_edit(neui_session_t session, neui_widget_t widget,
                                           int row, int col)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g) return;
    if (xpl_grid_try_begin_edit_pub(*g, row, col))
      grid_invalidate(s, g);
  }

  static void NEUI_ABI gr_end_cell_edit(neui_session_t session, neui_widget_t widget,
                                         bool commit)
  {
    Session* s = nullptr;
    auto* g = resolve_grid(session, widget, &s);
    if (!g || !g->model.edit.active) return;
    if (commit) (void)xpl_grid_commit_edit_pub(*g);
    else        xpl_grid_cancel_edit_pub(*g);
    grid_invalidate(s, g);
  }

  static bool NEUI_ABI gr_is_editing_cell(neui_session_t session, neui_widget_t widget,
                                            int* out_row, int* out_col)
  {
    auto* g = resolve_grid(session, widget);
    if (!g || !g->model.edit.active) {
      if (out_row) *out_row = -1;
      if (out_col) *out_col = -1;
      return false;
    }
    if (out_row) *out_row = g->model.edit.row;
    if (out_col) *out_col = g->model.edit.col;
    return true;
  }

  neui_grid_api_t grid_api = {
    NEUI_VERSION,
    gr_add_column,
    gr_get_column_count,
    gr_set_column_width,
    gr_get_column_width,
    gr_set_column_min_width,
    gr_set_column_align,
    gr_set_column_header,
    gr_get_column_header,
    gr_remove_column,
    gr_clear_columns,
    gr_add_row,
    gr_get_row_count,
    gr_remove_row,
    gr_clear_rows,
    gr_set_cell_text,
    gr_get_cell_text,
    gr_set_cell_color,
    gr_set_cell_enabled,
    gr_clear_cell_overrides,
    gr_set_selected_row,
    gr_get_selected_row,
    gr_set_selected_cell,
    gr_get_selected_cell,
    gr_ensure_row_visible,
    gr_ensure_cell_visible,
    gr_set_scroll_x,
    gr_get_scroll_x,
    gr_hit_test,
    gr_set_column_sortable,
    gr_set_column_sort_kind,
    gr_set_sort,
    gr_add_sort,
    gr_clear_sort,
    gr_get_sort_count,
    gr_get_sort_level,
    gr_logical_to_visual_row,
    gr_visual_to_logical_row,
    gr_set_column_editable,
    gr_get_column_editable,
    gr_begin_cell_edit,
    gr_end_cell_edit,
    gr_is_editing_cell,
  };

  // -------------------------------------------------------------------------
  // DnD API (NEUI_API_DND). Drop-target only in v1.

  static void NEUI_ABI dnd_set_drop_target(neui_session_t session,
                                            neui_widget_t widget, bool enable)
  {
    auto* s = get_session(session);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    s->_widgets[idx].drop_target = enable;
  }

  static bool NEUI_ABI dnd_get_drop_target(neui_session_t session,
                                            neui_widget_t widget)
  {
    auto* s = get_session(session);
    if (!s) return false;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return false;
    return s->_widgets[idx].drop_target;
  }

  static void NEUI_ABI dnd_set_accepted_formats(neui_session_t session,
                                                 neui_widget_t widget,
                                                 const char* const* mimes,
                                                 int count)
  {
    auto* s = get_session(session);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& w = s->_widgets[idx];
    w.accepted_mimes.clear();
    if (mimes && count > 0) {
      w.accepted_mimes.reserve(static_cast<size_t>(count));
      for (int i = 0; i < count; ++i) {
        if (mimes[i]) w.accepted_mimes.emplace_back(mimes[i]);
      }
    }
  }

  static void NEUI_ABI dnd_accept(neui_session_t session,
                                   neui_dnd_action_t action)
  {
    auto* s = get_session(session);
    if (!s) return;
    if (!s->_in_dnd_dispatch) return;  // only valid inside a DnD callback
    s->_last_accepted_action = static_cast<uint32_t>(action);
  }

  // Shared worker for both public entry points. asset_none + (-1, -1) =
  // no preview.
  static neui_dnd_action_t dnd_begin_drag_impl(neui_session_t session,
                                                 neui_widget_t source_widget,
                                                 neui_data_item_t payload,
                                                 uint32_t allowed_actions,
                                                 neui_asset_t preview_asset,
                                                 int hot_x, int hot_y)
  {
    auto* s = get_session_for_widget(session, source_widget);
    if (!s) return NEUI_DND_ACTION_NONE;
    if (s->_in_dnd_dispatch) return NEUI_DND_ACTION_NONE;    // no re-entry
    if (s->_drag_source_active) return NEUI_DND_ACTION_NONE; // one drag at a time
    auto* item = s->_data_items.get(payload.id);
    if (!item) return NEUI_DND_ACTION_NONE;
    void* native = s->find_parent_native_handle(WidgetToIndex(source_widget));
    if (!native) {
      // Source widget itself might be the frame.
      auto* wd = s->get_widget(WidgetToIndex(source_widget));
      if (wd) native = wd->native_handle;
    }
    if (!native) return NEUI_DND_ACTION_NONE;

    // Resolve preview asset to a per-platform native bitmap. Degrades to
    // no-preview on any failure (wrong session, COMPOUND/BEHAVIOR kind,
    // platform builder returns null).
    void* preview_native = nullptr;
    if (preview_asset.id != asset_none.id) {
      uint32_t a_sess = (preview_asset.id >> 16) & 0xffff;
      if (a_sess == (s->get_session_id() & 0xffff)) {
        const uint8_t* bgra = nullptr;
        uint32_t       w_px = 0, h_px = 0;
        float          scale = 1.0f;
        if (s->_asset_manager.get_pixels_for_export(preview_asset.id & 0xffff,
                                                      &bgra, &w_px, &h_px,
                                                      &scale)) {
          preview_native = platform_make_drag_preview(bgra, w_px, h_px, scale);
        }
      }
    }

    s->_drag_source_active = true;
    uint32_t r = platform_dnd_begin_drag(native, item, allowed_actions,
                                          preview_native, hot_x, hot_y);
    s->_drag_source_active = false;
    return static_cast<neui_dnd_action_t>(r);
  }

  static neui_dnd_action_t NEUI_ABI dnd_begin_drag(neui_session_t session,
                                                    neui_widget_t source_widget,
                                                    neui_data_item_t payload,
                                                    uint32_t allowed_actions)
  {
    return dnd_begin_drag_impl(session, source_widget, payload,
                                allowed_actions, asset_none, -1, -1);
  }

  static neui_dnd_action_t NEUI_ABI dnd_begin_drag_with_preview(
                                                    neui_session_t session,
                                                    neui_widget_t source_widget,
                                                    neui_data_item_t payload,
                                                    uint32_t allowed_actions,
                                                    const neui_drag_preview_t* preview)
  {
    if (!preview) {
      return dnd_begin_drag_impl(session, source_widget, payload,
                                  allowed_actions, asset_none, -1, -1);
    }
    return dnd_begin_drag_impl(session, source_widget, payload,
                                allowed_actions, preview->image,
                                preview->hot_x, preview->hot_y);
  }

  neui_dnd_api_t dnd_api = {
    NEUI_VERSION,
    dnd_set_drop_target,
    dnd_get_drop_target,
    dnd_set_accepted_formats,
    dnd_accept,
    dnd_begin_drag,
    dnd_begin_drag_with_preview,
  };

  // Non-static wrapper so host.cpp's BehaviorDispatchCtx::begin_drag can
  // reach the file-static dnd_begin_drag_with_preview from a sibling TU.
  // Receives the CustomDrawWidget pointer the dispatch carries as
  // host_data, unpacks it into the public API shape, and forwards.
  uint32_t xpl_behavior_begin_drag(void* host_data,
                                     neui_data_item_t item,
                                     uint32_t allowed_actions,
                                     uint32_t preview_image,
                                     int hot_x, int hot_y)
  {
    auto* wd = static_cast<WidgetData*>(host_data);
    if (!wd) return NEUI_DND_ACTION_NONE;
    neui_session_t sess = { wd->session_id };
    neui_widget_t  wid  = { wd->widget_id };
    neui_drag_preview_t preview = { { preview_image }, hot_x, hot_y };
    return static_cast<uint32_t>(
      dnd_begin_drag_with_preview(sess, wid, item, allowed_actions,
                                    preview_image ? &preview : nullptr));
  }

} // namespace xpl_host
