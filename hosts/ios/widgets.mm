// Native iOS host - all the public API tables (widgets / items / tree / attrs /
// clipboard / commands / scroll / tabs / asset / compound / behavior / dnd /
// grid / notify).
//
// MILESTONE 7. Structural mirror of hosts/macos/widgets.mm, scoped to the v1
// core subset. The platform-neutral tables (attrs / clipboard / scroll / asset
// / compound / behavior) are direct ports - they only touch Session /
// WidgetData / the shared hosts/shared/*.h helpers. The widget/items/tree
// tables are model-only for the stubbed types (LISTBOX / COMBOBOX / TREEVIEW
// store items + tree nodes but no native control). Native control creation,
// per-widget text/value get-set, the painted-view paint pass, and frame
// lifecycle live in window.mm (the UIKit-coupled half).
//
// AppKit -> UIKit deltas are kept inside the window.mm helpers (declared
// below); this TU stays mostly pure C++.

#import <UIKit/UIKit.h>

#include "host.h"
#include "../shared/compound.h"
#include "../../backends/cg/cg_backend.h"

#include "../shared/ios/clipboard_ios.h"
#include "../shared/ios/message_box_ios.h"

#include <algorithm>
#include <cstring>

namespace ios_host
{
  // Widget id layout: upper 16 = owning session id, lower 16 = tree slot
  // (mirror of every other host).
  static uint32_t WidgetToIndex(neui_widget_t widget) { return widget.id & 0xffff; }
  static neui_widget_t IndexToWidget(uint32_t session_id, uint32_t idx)
  {
    return { ((session_id & 0xffff) << 16) | (idx & 0xffff) };
  }

  static Session* get_session(neui_session_t session)
  {
    uint32_t idx = (session.session & 0xffff) - 1;
    if (idx < sessions.size()) return sessions[idx].get();
    return nullptr;
  }
  static bool widget_belongs_to_session(neui_widget_t widget, uint32_t session_id)
  {
    if (widget.id == 0)          return true;   // widget_root
    if (widget.id == UINT32_MAX) return true;   // widget_none
    return ((widget.id >> 16) & 0xffff) == (session_id & 0xffff);
  }
  static Session* get_session_for_widget(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (!widget_belongs_to_session(widget, s->session_id())) return nullptr;
    return s;
  }

  // -------------------------------------------------------------------------
  // window.mm helpers (UIKit-coupled). Declared here so the C++ tables below
  // can drive native realization / text / geometry / paint.

  void  realize_widget_ios(Session* s, uint32_t idx);
  void  release_native_window_ios(WidgetData& wd);
  void  release_native_control_ios(WidgetData& wd);
  void  mark_widget_dirty_for_paint(WidgetData& wd);
  void  apply_enabled_native_ios(WidgetData& wd);
  void  apply_geometry_native_ios(WidgetData& wd);
  void  apply_native_font_ios(WidgetData& wd);
  void  set_native_text_ios(WidgetData& wd, const char* text);
  int   get_native_text_ios(WidgetData& wd, char* buf, int buflen); // -1 = no native text
  void  set_native_check_ios(WidgetData& wd, neui_check_state_t state);
  void  set_native_float_ios(WidgetData& wd, const char* key, float value);
  void  set_window_title_ios(WidgetData& wd, const char* text);
  bool  invoke_focused_command_ios(uint32_t cmd);
  void  section_refresh_scroll_state_ios(WidgetData& wd);
  void  section_apply_layout_changes_ios(WidgetData& sec);
  void  section_ensure_body_view_ios(WidgetData& sec);
  void  section_reposition_children_ios(WidgetData& sec);
  void  section_notify_scroll_changed_ios(WidgetData& wd);
  void  tabview_collect_pages_ios(WidgetData& tv, std::vector<uint32_t>& out);
  void  tabview_select_ios(WidgetData& tv, int new_index);
  void  tabview_apply_page_geometry_ios(WidgetData& tv);
  void  frame_refresh_hamburger_ios(WidgetData& frame);
  // LISTBOX / TREEVIEW (native UITableView) model -> view sync. reload_*_ios
  // reloadData the table; tree_rebuild_visible_rows_ios re-flattens the
  // expanded-tree model first (no-op when there is no native control yet).
  void  reload_native_items_ios(WidgetData& wd);
  void  reload_native_tree_ios(WidgetData& wd);
  void  reload_native_item_selection_ios(WidgetData& wd);
  void  reload_native_tree_selection_ios(WidgetData& wd);
  void  tree_rebuild_visible_rows_ios(WidgetData& wd);
  int   notify_message_box_ios(WidgetData& frame, const char* text,
                               const char* caption, uint32_t flags);
  void  notify_toast_ios(WidgetData& frame, const char* text);

  // Defined in window.mm - the painter draw_asset thunk + Session::widget_show.
  void NEUI_ABI ios_painter_draw_asset_thunk(void* host_token,
                                             neui_render_backend_t* backend,
                                             neui_render_ctx_t ctx,
                                             neui_asset_t asset,
                                             float x, float y, float w, float h,
                                             uint32_t frame,
                                             uint32_t tint);

  // A TABPAGE is a chip-less SECTION: the section attr handlers (background /
  // align / scroll-mode / content extent) treat both alike.
  static bool is_section_like_w(const char* type)
  {
    return type && (!strcmp(type, NEUI_W_SECTION) ||
                    !strcmp(type, NEUI_W_TABPAGE));
  }
  // The TABVIEW parent of `wd` if `wd` is a TABPAGE, else nullptr. Used to
  // repaint the chip strip when a page's tab label / chip colours change.
  static WidgetData* tabview_parent_of_page(WidgetData& wd)
  {
    if (!wd.session || !wd.type || strcmp(wd.type, NEUI_W_TABPAGE) != 0)
      return nullptr;
    uint32_t pidx = wd.session->_widgets.get_parent(wd.index);
    if (!pidx || !wd.session->_widgets.exists(pidx)) return nullptr;
    auto& pw = wd.session->_widgets[pidx];
    if (pw.type && !strcmp(pw.type, NEUI_W_TABVIEW)) return &pw;
    return nullptr;
  }
  static bool is_font_attr(const char* key)
  {
    return key && (!strcmp(key, NEUI_ATTR_FONT_FAMILY) ||
                   !strcmp(key, NEUI_ATTR_FONT_SIZE) ||
                   !strcmp(key, NEUI_ATTR_FONT_WEIGHT));
  }

  // -------------------------------------------------------------------------
  // Session::widget_create / widget_destroy - pure C++ tree manipulation
  // (identical shape to the macOS host).

  neui_widget_t Session::widget_create(neui_widget_t parent, const char* type,
                                       int x, int y, int width, int height,
                                       void* userdata)
  {
    auto wd = std::make_unique<WidgetData>();
    wd->type       = type;
    wd->x = x; wd->y = y; wd->width = width; wd->height = height;
    wd->visible    = true;
    wd->userdata   = userdata;
    wd->session    = this;
    wd->session_id = _session_id;

    wd->isroot = (parent.id == widget_none.id) ||
                 (type && (!strcmp(type, NEUI_W_APPWINDOW) ||
                           !strcmp(type, NEUI_W_PLUGWINDOW) ||
                           !strcmp(type, NEUI_W_DIALOG)));

    if (type && (!strcmp(type, NEUI_W_BUTTON) || !strcmp(type, NEUI_W_INPUTBOX) ||
                 !strcmp(type, NEUI_W_CHECKBOX) || !strcmp(type, NEUI_W_CHECKBOX3) ||
                 !strcmp(type, NEUI_W_LISTBOX) || !strcmp(type, NEUI_W_COMBOBOX) ||
                 !strcmp(type, NEUI_W_MULTILINE) || !strcmp(type, NEUI_W_TREEVIEW) ||
                 !strcmp(type, NEUI_W_CUSTOMDRAW) || !strcmp(type, NEUI_W_KNOB) ||
                 !strcmp(type, NEUI_W_SLIDER) || !strcmp(type, NEUI_W_GRID) ||
                 // TABVIEW takes touches so its painted view can hit-test chip
                 // taps. TABPAGE is a chip-less SECTION - non-emit like SECTION
                 // (its scroll input is gated on section_scroll_state).
                 !strcmp(type, NEUI_W_TABVIEW)))
      wd->emit_events = true;

    if (type && !strcmp(type, NEUI_W_CHECKBOX3))
      neui_detail::ensure_attrs(wd->attrs).set_int(NEUI_ATTR_TRISTATE, 1);
    if (type && !strcmp(type, NEUI_W_MULTILINE))
      neui_detail::ensure_attrs(wd->attrs).set_int(NEUI_ATTR_MULTILINE, 1);

    uint32_t parent_idx = (parent.id == widget_none.id) ? 0 : WidgetToIndex(parent);
    uint32_t slot = _widgets.add_child(parent_idx, std::move(wd));
    _widgets[slot].index     = slot;
    _widgets[slot].widget_id = IndexToWidget(_session_id, slot).id;

    // MENUBAR is a model-only widget (drives the hamburger UIMenu); no view.
    if (type && !strcmp(type, NEUI_W_MENUBAR))
      return IndexToWidget(_session_id, slot);

    // Post-show dynamic realization (no-op before widget_show).
    realize_widget_ios(this, slot);
    return IndexToWidget(_session_id, slot);
  }

  void Session::widget_destroy(neui_widget_t widget)
  {
    uint32_t idx = WidgetToIndex(widget);
    if (!_widgets.exists(idx)) return;

    // Depth-first: destroy children first.
    uint32_t child = _widgets.child(idx);
    while (child != 0) {
      uint32_t nxt = _widgets.next(child);
      widget_destroy(neui_widget_t{ _widgets[child].widget_id });
      child = nxt;
    }

    auto& wd = _widgets[idx];
    if (_client_widget_api && _client_widget_api->ondestroy)
      _client_widget_api->ondestroy(_token, neui_widget_t{ wd.widget_id }, wd.userdata);

    if (wd.image_asset_owned && wd.image_asset.id != asset_none.id) {
      _asset_manager.release_slot(wd.image_asset.id & 0xffff,
                                  neui_cg_backend::get_backend());
      wd.image_asset = asset_none;
      wd.image_asset_owned = false;
    }

    // A destroyed TABPAGE drops a tab: re-flow the parent TABVIEW (the selected
    // index may now be out of range) + repaint its chip strip after the slot is
    // freed below.
    uint32_t tabview_parent = 0;
    if (wd.type && !strcmp(wd.type, NEUI_W_TABPAGE)) {
      uint32_t pidx = _widgets.get_parent(idx);
      if (pidx && _widgets.exists(pidx)) {
        auto& pw = _widgets[pidx];
        if (pw.type && !strcmp(pw.type, NEUI_W_TABVIEW))
          tabview_parent = pidx;
      }
    }

    if (wd.native_window)  release_native_window_ios(wd);
    if (wd.native_control) release_native_control_ios(wd);

    _widgets.remove(idx);

    if (tabview_parent && _widgets.exists(tabview_parent)) {
      auto& tv = _widgets[tabview_parent];
      tabview_apply_page_geometry_ios(tv);
      mark_widget_dirty_for_paint(tv);
    }
  }

  // -------------------------------------------------------------------------
  // neui_widget_api_t.

  static neui_widget_t NEUI_ABI w_create(neui_session_t session, neui_widget_t parent,
                                         const char* type, int x, int y,
                                         int width, int height, void* userdata)
  {
    auto* s = get_session_for_widget(session, parent);
    if (!s || !type) return widget_none;
    return s->widget_create(parent, type, x, y, width, height, userdata);
  }

  static void NEUI_ABI w_destroy(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (s) s->widget_destroy(widget);
  }

  static void NEUI_ABI w_hide(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    wd.visible = false;
    if (wd.native_window) {
      UIWindow* w = (__bridge UIWindow*)wd.native_window;
      w.hidden = YES;
    } else if (wd.native_control) {
      UIView* v = (__bridge UIView*)wd.native_control;
      v.hidden = YES;
    }
  }

  static void NEUI_ABI w_set_pos(neui_session_t session, neui_widget_t widget,
                                 int x, int y, int width, int height)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    wd.x = x; wd.y = y; wd.width = width; wd.height = height;
    apply_geometry_native_ios(wd);
  }

  static void NEUI_ABI w_set_size(neui_session_t session, neui_widget_t widget,
                                  int width, int height)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    wd.width = width; wd.height = height;
    apply_geometry_native_ios(wd);
  }

  static void NEUI_ABI w_set_emit_events(neui_session_t session, neui_widget_t widget,
                                         bool enabled)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (s->_widgets.exists(i)) s->_widgets[i].emit_events = enabled;
  }

  static void NEUI_ABI w_set_text(neui_session_t session, neui_widget_t widget,
                                  const char* text)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];

    // IMAGE: set_text supplies a file path; allocate an internally-owned bitmap
    // asset (clearing any previous one), last-set-wins with set_asset.
    if (wd.type && !strcmp(wd.type, NEUI_W_IMAGE)) {
      if (wd.image_asset_owned && wd.image_asset.id != asset_none.id)
        s->_asset_manager.release_slot(wd.image_asset.id & 0xffff,
                                       neui_cg_backend::get_backend());
      wd.image_asset = asset_none;
      wd.image_asset_owned = false;
      if (text && *text) {
        // Resolve @2x/@3x against the screen scale (every iOS device is Retina);
        // 0.0f would clamp to 1.0 and always pick the blurry 1x variant.
        float scale = (float)UIScreen.mainScreen.scale;
        if (scale <= 0) scale = 1.0f;
        uint32_t slot = s->_asset_manager.allocate_from_file(text, scale);
        if (slot != 0) {
          wd.image_asset = neui_asset_t{ ((s->session_id() & 0xffff) << 16) | (slot & 0xffff) };
          wd.image_asset_owned = true;
        }
      }
      wd.text = text ? text : "";
      mark_widget_dirty_for_paint(wd);
      return;
    }

    wd.text = text ? text : "";
    if (wd.native_window) {
      set_window_title_ios(wd, text);
    } else if (wd.native_control) {
      set_native_text_ios(wd, text);
    }
    // SECTION chip text: repaint. TABPAGE text is the tab label drawn by the
    // parent TABVIEW's chip strip - repaint the tabview, not the page.
    if (auto* tv = tabview_parent_of_page(wd)) mark_widget_dirty_for_paint(*tv);
    else if (is_section_like_w(wd.type)) mark_widget_dirty_for_paint(wd);
  }

  static int NEUI_ABI w_get_text(neui_session_t session, neui_widget_t widget,
                                 char* buf, int buflen)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return -1;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return -1;
    auto& wd = s->_widgets[i];

    // Pull live text from the native control where one exists.
    if (wd.native_control) {
      int n = get_native_text_ios(wd, buf, buflen);
      if (n >= 0) return n;
    }
    int needed = (int)wd.text.size() + 1;
    if (buf && buflen > 0) {
      int n = (buflen < needed) ? buflen : needed;
      memcpy(buf, wd.text.c_str(), (size_t)(n - 1));
      buf[n - 1] = '\0';
    }
    return needed;
  }

  static neui_widget_t NEUI_ABI w_get_first_child(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return widget_none;
    uint32_t i = (widget.id == 0) ? 0 : WidgetToIndex(widget);
    uint32_t c = s->_widgets.child(i);
    if (c == 0) return widget_none;
    return neui_widget_t{ s->_widgets[c].widget_id };
  }

  static neui_widget_t NEUI_ABI w_get_next_sibling(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return widget_none;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return widget_none;
    uint32_t n = s->_widgets.next(i);
    if (n == 0) return widget_none;
    return neui_widget_t{ s->_widgets[n].widget_id };
  }

  static void NEUI_ABI w_set_focus(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    if (wd.native_control) {
      UIView* v = (__bridge UIView*)wd.native_control;
      if ([v canBecomeFirstResponder]) [v becomeFirstResponder];
    }
  }

  static void NEUI_ABI w_set_check(neui_session_t session, neui_widget_t widget,
                                   neui_check_state_t state)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    neui_detail::ensure_attrs(wd.attrs).set_int("neui.ioshost.checkstate", (int)state);
    set_native_check_ios(wd, state);
  }

  static neui_check_state_t NEUI_ABI w_get_check(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return NEUI_CHECK_UNCHECKED;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return NEUI_CHECK_UNCHECKED;
    auto& wd = s->_widgets[i];
    if (wd.attrs)
      return (neui_check_state_t)wd.attrs->get_int("neui.ioshost.checkstate",
                                                   NEUI_CHECK_UNCHECKED);
    return NEUI_CHECK_UNCHECKED;
  }

  static void* NEUI_ABI w_get_native_handle(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return nullptr;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return nullptr;
    auto& wd = s->_widgets[i];
    return wd.native_window ? wd.native_window : wd.native_control;
  }

  static void NEUI_ABI w_set_tab_stop(neui_session_t session, neui_widget_t widget, bool enabled)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (s->_widgets.exists(i))
      neui_detail::ensure_attrs(s->_widgets[i].attrs).set_int(NEUI_ATTR_TAB_STOP, enabled ? 1 : 0);
  }

  static void NEUI_ABI w_set_owner(neui_session_t session, neui_widget_t dialog,
                                   neui_widget_t owner)
  {
    auto* s = get_session_for_widget(session, dialog);
    if (!s) return;
    uint32_t d = WidgetToIndex(dialog);
    if (!s->_widgets.exists(d)) return;
    s->_widgets[d].owner_index = (owner.id == 0 || owner.id == UINT32_MAX)
                                   ? 0 : WidgetToIndex(owner);
  }

  static void NEUI_ABI w_get_pos(neui_session_t session, neui_widget_t widget, int* x, int* y)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    if (x) *x = s->_widgets[i].x;
    if (y) *y = s->_widgets[i].y;
  }

  static void NEUI_ABI w_get_size(neui_session_t session, neui_widget_t widget, int* w, int* h)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    if (w) *w = s->_widgets[i].width;
    if (h) *h = s->_widgets[i].height;
  }

  static int NEUI_ABI w_popup_menu(neui_session_t /*session*/, neui_widget_t /*anchor*/,
                                   int /*x*/, int /*y*/, const char* const* /*items*/)
  {
    // TODO(ios phase 2): popup menu via UIMenu / UIEditMenuInteraction. The
    // only v1 consumer is the KNOB reset menu, which isn't a core-subset path.
    return 0;
  }

  static void NEUI_ABI w_invalidate(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    mark_widget_dirty_for_paint(s->_widgets[i]);
  }

  static void NEUI_ABI w_set_asset(neui_session_t session, neui_widget_t widget,
                                   neui_asset_t asset)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];

    // CUSTOMDRAW: kind-route to compound / behavior slots.
    if (wd.type && !strcmp(wd.type, NEUI_W_CUSTOMDRAW)) {
      if (asset.id != asset_none.id &&
          ((asset.id >> 16) & 0xffff) == (s->session_id() & 0xffff)) {
        auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
        if (e && e->kind == NEUI_ASSET_KIND_BEHAVIOR) { wd.behavior_asset = asset; return; }
        if (e && e->kind == NEUI_ASSET_KIND_COMPOUND) {
          wd.compound_asset = asset; mark_widget_dirty_for_paint(wd); return;
        }
        if (e && e->kind == NEUI_ASSET_KIND_COMPONENT) {
          // COMPONENT bundles compound + behavior + defaults: attach both
          // slots and stamp defaults (keys the widget lacks; client pre-sets win).
          wd.compound_asset = e->comp_compound;
          wd.behavior_asset = e->comp_behavior;
          auto& bag = neui_detail::ensure_attrs(wd.attrs);
          for (const auto& d : e->comp_defaults) {
            if (bag.has(d.key)) continue;
            switch (d.type) {
              case neui_detail::ComponentDefaultAttr::INT:    bag.set_int(d.key, d.ival); break;
              case neui_detail::ComponentDefaultAttr::FLOAT:  bag.set_float(d.key, d.fval); break;
              case neui_detail::ComponentDefaultAttr::STRING: bag.set_string(d.key, d.sval.c_str()); break;
            }
          }
          mark_widget_dirty_for_paint(wd);
          return;
        }
      } else if (asset.id == asset_none.id) {
        wd.compound_asset = asset_none;
        wd.behavior_asset = asset_none;
        mark_widget_dirty_for_paint(wd);
        return;
      }
    }

    // IMAGE: bind a client-owned asset (clears any internally-owned one).
    if (wd.type && !strcmp(wd.type, NEUI_W_IMAGE)) {
      if (wd.image_asset_owned && wd.image_asset.id != asset_none.id)
        s->_asset_manager.release_slot(wd.image_asset.id & 0xffff,
                                       neui_cg_backend::get_backend());
      wd.image_asset = asset;
      wd.image_asset_owned = false;
      mark_widget_dirty_for_paint(wd);
    }
  }

  // Instantiate a CUSTOMDRAW from a COMPONENT asset (create + COMPONENT-aware
  // set_asset, which attaches both slots + stamps defaults). Mirrors macOS.
  static neui_widget_t NEUI_ABI w_create_from_component(neui_session_t session,
      neui_widget_t parent, neui_asset_t component, int x, int y, int width, int height)
  {
    auto* s = get_session_for_widget(session, parent);
    if (!s || component.id == asset_none.id) return widget_none;
    if (((component.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return widget_none;
    auto* ce = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!ce || ce->kind != NEUI_ASSET_KIND_COMPONENT) return widget_none;
    if (width  <= 0) width  = static_cast<int>(ce->comp_w + 0.5f);
    if (height <= 0) height = static_cast<int>(ce->comp_h + 0.5f);
    neui_widget_t w = w_create(session, parent, NEUI_W_CUSTOMDRAW, x, y, width, height, nullptr);
    if (w.id == widget_none.id) return widget_none;
    w_set_asset(session, w, component);
    return w;
  }

  static void NEUI_ABI w_set_enabled(neui_session_t session, neui_widget_t widget, bool enabled)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    wd.enabled = enabled;
    apply_enabled_native_ios(wd);
  }

  static bool NEUI_ABI w_get_enabled(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return true;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return true;
    return s->_widgets[i].enabled;
  }

  // Defined in window.mm (needs the frame's UIView safe-area insets).
  void widget_client_rect_ios(Session* s, uint32_t widget_idx,
                              int* x, int* y, int* w, int* h);

  static void NEUI_ABI w_get_client_rect(neui_session_t session, neui_widget_t widget,
                                         int* x, int* y, int* width, int* height)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    widget_client_rect_ios(s, i, x, y, width, height);
  }

  // Session::widget_show is defined in window.mm.
  static void NEUI_ABI w_show(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (s) s->widget_show(widget);
  }

  neui_widget_api_t widgets_api = {
    w_create, w_destroy, w_show, w_hide,
    w_set_pos, w_set_size, w_set_emit_events,
    w_set_text, w_get_text,
    w_get_first_child, w_get_next_sibling,
    w_set_focus,
    w_set_check, w_get_check,
    w_get_native_handle,
    w_set_tab_stop,
    w_set_owner,
    w_get_pos, w_get_size,
    w_popup_menu,
    w_invalidate,
    w_set_asset,
    w_set_enabled,
    w_get_enabled,
    w_get_client_rect,
    w_create_from_component,
  };

  // -------------------------------------------------------------------------
  // Items API - drives the LISTBOX native UITableView (reload on every mutation;
  // model + reload also covers COMBOBOX, which is still a phase-2 stub with no
  // native control, so reload_native_items_ios is a no-op there).

  static void NEUI_ABI i_clear(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    s->_widgets[i].items.clear();
    s->_widgets[i].selected_item = NEUI_ITEM_NONE;
    reload_native_items_ios(s->_widgets[i]);
  }
  static uint32_t NEUI_ABI i_add(neui_session_t session, neui_widget_t widget,
                                 const char* text, void* userdata)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return NEUI_ITEM_NONE;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return NEUI_ITEM_NONE;
    auto& wd = s->_widgets[i];
    WidgetData::ItemEntry e;
    e.text = text ? text : ""; e.userdata = userdata;
    wd.items.push_back(std::move(e));
    reload_native_items_ios(wd);
    return (uint32_t)(wd.items.size() - 1);
  }
  static void NEUI_ABI i_remove(neui_session_t session, neui_widget_t widget, uint32_t idx)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    if (idx >= wd.items.size()) return;
    wd.items.erase(wd.items.begin() + (ptrdiff_t)idx);
    if (wd.selected_item != NEUI_ITEM_NONE) {
      if (wd.selected_item == idx) wd.selected_item = NEUI_ITEM_NONE;
      else if (wd.selected_item > idx) --wd.selected_item;
    }
    reload_native_items_ios(wd);
  }
  static uint32_t NEUI_ABI i_count(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return 0;
    return (uint32_t)s->_widgets[i].items.size();
  }
  static int NEUI_ABI i_get_text(neui_session_t session, neui_widget_t widget,
                                 uint32_t idx, char* buf, int buflen)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return 0;
    auto& wd = s->_widgets[i];
    if (idx >= wd.items.size()) return 0;
    const auto& t = wd.items[idx].text;
    int needed = (int)t.size() + 1;
    if (buf && buflen > 0) {
      int n = (buflen < needed) ? buflen : needed;
      memcpy(buf, t.c_str(), (size_t)(n - 1)); buf[n - 1] = '\0';
    }
    return needed;
  }
  static void NEUI_ABI i_set_text(neui_session_t session, neui_widget_t widget,
                                  uint32_t idx, const char* text)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    if (idx < wd.items.size()) {
      wd.items[idx].text = text ? text : "";
      reload_native_items_ios(wd);
    }
  }
  static void* NEUI_ABI i_get_userdata(neui_session_t session, neui_widget_t widget, uint32_t idx)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return nullptr;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return nullptr;
    auto& wd = s->_widgets[i];
    return idx < wd.items.size() ? wd.items[idx].userdata : nullptr;
  }
  static uint32_t NEUI_ABI i_get_selected(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return NEUI_ITEM_NONE;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return NEUI_ITEM_NONE;
    return s->_widgets[i].selected_item;
  }
  static void NEUI_ABI i_set_selected(neui_session_t session, neui_widget_t widget, uint32_t idx)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    if (idx != NEUI_ITEM_NONE && idx >= wd.items.size()) return;
    wd.selected_item = idx;
    reload_native_item_selection_ios(wd);
  }

  neui_items_api_t items_api = {
    i_clear, i_add, i_remove, i_count,
    i_get_text, i_set_text, i_get_userdata,
    i_get_selected, i_set_selected,
  };

  // -------------------------------------------------------------------------
  // Tree API - the MENUBAR model (drives the hamburger UIMenu, rebuilt in
  // window.mm from this model). TREEVIEW is a phase-2 stub (model only).

  static bool widget_is_menubar(WidgetData& wd)
  {
    return wd.type && !strcmp(wd.type, NEUI_W_MENUBAR);
  }

  static neui_item_t NEUI_ABI t_add(neui_session_t session, neui_widget_t widget,
                                    neui_item_t parent, const char* text, void* userdata)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return tree_item_none;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return tree_item_none;
    auto& wd = s->_widgets[i];

    uint32_t id = wd.next_tree_id++;
    WidgetData::TreeNode node;
    node.parent_id = (parent.id == tree_item_root.id) ? 0 : parent.id;
    node.text      = text ? text : "";
    node.userdata  = userdata;
    wd.tree_items[id] = node;
    wd.tree_items_ordered.push_back(id);

    if (widget_is_menubar(wd)) frame_refresh_hamburger_ios(wd);
    else                       reload_native_tree_ios(wd);  // TREEVIEW
    return neui_item_t{ id };
  }
  static void NEUI_ABI t_remove(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    wd.tree_items.erase(item.id);
    auto& v = wd.tree_items_ordered;
    v.erase(std::remove(v.begin(), v.end(), item.id), v.end());
    if (wd.selected_tree_item == item.id) wd.selected_tree_item = UINT32_MAX;
    if (widget_is_menubar(wd)) frame_refresh_hamburger_ios(wd);
    else                       reload_native_tree_ios(wd);  // TREEVIEW
  }
  static void NEUI_ABI t_clear(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    wd.tree_items.clear();
    wd.tree_items_ordered.clear();
    wd.next_tree_id = 1;
    wd.selected_tree_item = UINT32_MAX;
    if (widget_is_menubar(wd)) frame_refresh_hamburger_ios(wd);
    else                       reload_native_tree_ios(wd);  // TREEVIEW
  }
  static int NEUI_ABI t_get_text(neui_session_t session, neui_widget_t widget,
                                 neui_item_t item, char* buf, int buflen)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return 0;
    auto& wd = s->_widgets[i];
    auto it = wd.tree_items.find(item.id);
    if (it == wd.tree_items.end()) return 0;
    const auto& t = it->second.text;
    int needed = (int)t.size() + 1;
    if (buf && buflen > 0) {
      int n = (buflen < needed) ? buflen : needed;
      memcpy(buf, t.c_str(), (size_t)(n - 1)); buf[n - 1] = '\0';
    }
    return needed;
  }
  static void NEUI_ABI t_set_text(neui_session_t session, neui_widget_t widget,
                                  neui_item_t item, const char* text)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    auto it = wd.tree_items.find(item.id);
    if (it != wd.tree_items.end()) {
      it->second.text = text ? text : "";
      if (widget_is_menubar(wd)) frame_refresh_hamburger_ios(wd);
      else                       reload_native_tree_ios(wd);  // TREEVIEW
    }
  }
  static void* NEUI_ABI t_get_userdata(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return nullptr;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return nullptr;
    auto& wd = s->_widgets[i];
    auto it = wd.tree_items.find(item.id);
    return it != wd.tree_items.end() ? it->second.userdata : nullptr;
  }
  static void NEUI_ABI t_set_enabled(neui_session_t session, neui_widget_t widget,
                                     neui_item_t item, bool enabled)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    auto it = wd.tree_items.find(item.id);
    if (it != wd.tree_items.end()) {
      it->second.enabled = enabled;
      if (widget_is_menubar(wd)) frame_refresh_hamburger_ios(wd);
    }
  }
  static bool NEUI_ABI t_get_enabled(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return false;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return false;
    auto& wd = s->_widgets[i];
    auto it = wd.tree_items.find(item.id);
    return it != wd.tree_items.end() ? it->second.enabled : false;
  }
  static void NEUI_ABI t_set_checked(neui_session_t session, neui_widget_t widget,
                                     neui_item_t item, bool checked)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    auto it = wd.tree_items.find(item.id);
    if (it != wd.tree_items.end()) {
      it->second.checked = checked;
      // The UIMenu is rebuilt from the model; refreshing re-emits the element
      // with its checkmark state.
      if (widget_is_menubar(wd)) frame_refresh_hamburger_ios(wd);
    }
  }
  static bool NEUI_ABI t_get_checked(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return false;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return false;
    auto& wd = s->_widgets[i];
    auto it = wd.tree_items.find(item.id);
    return it != wd.tree_items.end() ? it->second.checked : false;
  }
  static void NEUI_ABI t_set_shortcut(neui_session_t session, neui_widget_t widget,
                                      neui_item_t item, uint32_t mods, uint32_t key)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    auto it = wd.tree_items.find(item.id);
    if (it != wd.tree_items.end()) {
      it->second.shortcut_mods = mods;
      it->second.shortcut_key  = key;
      if (widget_is_menubar(wd)) frame_refresh_hamburger_ios(wd);
    }
  }
  static neui_item_t NEUI_ABI t_get_first_child(neui_session_t session, neui_widget_t widget,
                                                neui_item_t parent)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return tree_item_none;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return tree_item_none;
    auto& wd = s->_widgets[i];
    uint32_t parent_id = (parent.id == tree_item_root.id) ? 0 : parent.id;
    for (uint32_t id : wd.tree_items_ordered) {
      auto it = wd.tree_items.find(id);
      if (it != wd.tree_items.end() && it->second.parent_id == parent_id)
        return neui_item_t{ id };
    }
    return tree_item_none;
  }
  static neui_item_t NEUI_ABI t_get_next_sibling(neui_session_t session, neui_widget_t widget,
                                                 neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return tree_item_none;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return tree_item_none;
    auto& wd = s->_widgets[i];
    auto cur = wd.tree_items.find(item.id);
    if (cur == wd.tree_items.end()) return tree_item_none;
    uint32_t parent_id = cur->second.parent_id;
    bool found = false;
    for (uint32_t id : wd.tree_items_ordered) {
      if (found) {
        auto it = wd.tree_items.find(id);
        if (it != wd.tree_items.end() && it->second.parent_id == parent_id)
          return neui_item_t{ id };
      }
      if (id == item.id) found = true;
    }
    return tree_item_none;
  }
  static neui_item_t NEUI_ABI t_get_selected(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return tree_item_none;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return tree_item_none;
    uint32_t sel = s->_widgets[i].selected_tree_item;
    return sel == UINT32_MAX ? tree_item_none : neui_item_t{ sel };
  }
  static void NEUI_ABI t_set_selected(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    wd.selected_tree_item = item.id;
    // Reflect into the native UITableView: find the row for this item in the
    // visible model + select it. Off-screen (collapsed-ancestor) items have no
    // row, so the selection is model-only until they become visible.
    if (wd.native_control && wd.type && !strcmp(wd.type, NEUI_W_TREEVIEW)) {
      tree_rebuild_visible_rows_ios(wd);  // keep the row map fresh
      reload_native_tree_selection_ios(wd);
    }
  }
  static void NEUI_ABI t_set_menu_cmd(neui_session_t session, neui_widget_t widget,
                                      neui_item_t item, uint32_t cmd)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return;
    auto& wd = s->_widgets[i];
    auto it = wd.tree_items.find(item.id);
    if (it != wd.tree_items.end()) it->second.menu_cmd = cmd;
  }

  neui_tree_api_t tree_api = {
    t_add, t_remove, t_clear,
    t_get_text, t_set_text, t_get_userdata,
    t_set_enabled, t_get_enabled,
    t_set_shortcut,
    t_get_first_child, t_get_next_sibling,
    t_get_selected, t_set_selected,
    t_set_menu_cmd,
    t_set_checked, t_get_checked,
  };

  // -------------------------------------------------------------------------
  // Attribute API. Pure AttrBag get/set + live-application hooks.

  static int NEUI_ABI a_set_int(neui_session_t session, neui_widget_t widget,
                                const char* key, int32_t value)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return 0;
    auto& wd = s->_widgets[i];
    neui_detail::ensure_attrs(wd.attrs).set_int(key, value);
    if (wd.type && !strcmp(wd.type, NEUI_W_CUSTOMDRAW) &&
        wd.compound_asset.id != asset_none.id)
      mark_widget_dirty_for_paint(wd);
    if (is_section_like_w(wd.type) && !strcmp(key, NEUI_ATTR_BACKGROUND)) {
      mark_widget_dirty_for_paint(wd);
      // The active page's background drives the TABVIEW body fill + active-chip
      // colour, so repaint the parent strip too.
      if (auto* tv = tabview_parent_of_page(wd)) mark_widget_dirty_for_paint(*tv);
    }
    if (is_section_like_w(wd.type) &&
        (!strcmp(key, NEUI_ATTR_CONTENT_WIDTH) || !strcmp(key, NEUI_ATTR_CONTENT_HEIGHT)))
      section_apply_layout_changes_ios(wd);
    // TABPAGE chip colours -> repaint the parent TABVIEW's strip.
    if (wd.type && !strcmp(wd.type, NEUI_W_TABPAGE) &&
        (!strcmp(key, NEUI_ATTR_TAB_CHIP_BG_COLOR) ||
         !strcmp(key, NEUI_ATTR_TAB_CHIP_TEXT_COLOR)))
      if (auto* tv = tabview_parent_of_page(wd)) mark_widget_dirty_for_paint(*tv);
    // TABVIEW style attrs (strip size, border, chip radius, strip bg) -> re-flow
    // + repaint (the paint pass re-applies page geometry when the body changed).
    if (wd.type && !strcmp(wd.type, NEUI_W_TABVIEW) &&
        (!strcmp(key, NEUI_ATTR_TAB_STRIP_SIZE) ||
         !strcmp(key, NEUI_ATTR_TAB_BORDER_COLOR) ||
         !strcmp(key, NEUI_ATTR_TAB_BORDER_WIDTH) ||
         !strcmp(key, NEUI_ATTR_TAB_CHIP_RADIUS) ||
         !strcmp(key, NEUI_ATTR_TAB_STRIP_BG_COLOR)))
      mark_widget_dirty_for_paint(wd);
    if (is_font_attr(key)) { mark_widget_dirty_for_paint(wd); apply_native_font_ios(wd); }
    return 1;
  }
  static int32_t NEUI_ABI a_get_int(neui_session_t session, neui_widget_t widget,
                                    const char* key, int32_t def)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return def;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i) || !s->_widgets[i].attrs) return def;
    return s->_widgets[i].attrs->get_int(key, def);
  }
  static int NEUI_ABI a_set_string(neui_session_t session, neui_widget_t widget,
                                   const char* key, const char* value)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return 0;
    auto& wd = s->_widgets[i];
    neui_detail::ensure_attrs(wd.attrs).set_string(key, value);
    if (wd.type && !strcmp(wd.type, NEUI_W_CUSTOMDRAW) &&
        wd.compound_asset.id != asset_none.id)
      mark_widget_dirty_for_paint(wd);
    if (is_section_like_w(wd.type) && !strcmp(key, NEUI_ATTR_ALIGN_TEXT)) {
      section_ensure_body_view_ios(wd);
      section_apply_layout_changes_ios(wd);
    }
    if (is_section_like_w(wd.type) && !strcmp(key, NEUI_ATTR_SCROLL_MODE)) {
      section_refresh_scroll_state_ios(wd);
      section_ensure_body_view_ios(wd);
      if (wd.section_scroll_state) {
        auto& st = *wd.section_scroll_state;
        st.scroll_x = st.scroll_y = 0;
        st.kin_v = st.kin_h = neui_detail::ScrollKinetics{};
        st.kinetic_over_v = st.kinetic_over_h = false;
      }
      section_apply_layout_changes_ios(wd);
    }
    // TABVIEW: NEUI_ATTR_TAB_POSITION changes the strip edge + content body
    // rect; a repaint re-flows the chips + re-sizes the selected page.
    if (wd.type && !strcmp(wd.type, NEUI_W_TABVIEW) &&
        !strcmp(key, NEUI_ATTR_TAB_POSITION))
      mark_widget_dirty_for_paint(wd);
    if (is_font_attr(key)) { mark_widget_dirty_for_paint(wd); apply_native_font_ios(wd); }
    return 1;
  }
  static const char* NEUI_ABI a_get_string(neui_session_t session, neui_widget_t widget,
                                           const char* key)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return nullptr;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i) || !s->_widgets[i].attrs) return nullptr;
    return s->_widgets[i].attrs->get_string(key);
  }
  static int NEUI_ABI a_has(neui_session_t session, neui_widget_t widget, const char* key)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i) || !s->_widgets[i].attrs) return 0;
    return s->_widgets[i].attrs->has(key) ? 1 : 0;
  }
  static int NEUI_ABI a_remove(neui_session_t session, neui_widget_t widget, const char* key)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i) || !s->_widgets[i].attrs) return 0;
    return s->_widgets[i].attrs->remove(key) ? 1 : 0;
  }
  static int NEUI_ABI a_set_float(neui_session_t session, neui_widget_t widget,
                                  const char* key, float value)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return 0;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return 0;
    auto& wd = s->_widgets[i];
    neui_detail::ensure_attrs(wd.attrs).set_float(key, value);
    if (wd.native_control) set_native_float_ios(wd, key, value);
    if (wd.type && !strcmp(wd.type, NEUI_W_CUSTOMDRAW) &&
        wd.compound_asset.id != asset_none.id)
      mark_widget_dirty_for_paint(wd);
    if (is_font_attr(key)) { mark_widget_dirty_for_paint(wd); apply_native_font_ios(wd); }
    return 1;
  }
  static float NEUI_ABI a_get_float(neui_session_t session, neui_widget_t widget,
                                    const char* key, float def)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s || !key) return def;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i) || !s->_widgets[i].attrs) return def;
    return s->_widgets[i].attrs->get_float(key, def);
  }
  static int NEUI_ABI a_set_session_int(neui_session_t session, const char* key, int32_t value)
  {
    auto* s = get_session(session);
    if (!s || !key) return 0;
    neui_detail::ensure_attrs(s->_session_attrs).set_int(key, value);
    return 1;
  }
  static int32_t NEUI_ABI a_get_session_int(neui_session_t session, const char* key, int32_t d)
  {
    auto* s = get_session(session);
    if (!s || !key || !s->_session_attrs) return d;
    return s->_session_attrs->get_int(key, d);
  }

  neui_attr_api_t attrs_api = {
    NEUI_VERSION,
    a_set_int, a_get_int,
    a_set_string, a_get_string,
    a_has, a_remove,
    a_set_float, a_get_float,
    a_set_session_int, a_get_session_int,
  };

  // -------------------------------------------------------------------------
  // Clipboard API (UIPasteboard via clipboard_ios.h).

  static int  NEUI_ABI c_set_text(neui_session_t, const char* utf8)
  { return neui_detail::clipboard_set_text_ios(utf8, utf8 ? (uint32_t)strlen(utf8) : 0) ? 1 : 0; }
  static int  NEUI_ABI c_get_text(neui_session_t, char* buf, int buflen)
  { return neui_detail::clipboard_get_text_ios(buf, buflen); }
  static bool NEUI_ABI c_has_text(neui_session_t)
  { return neui_detail::clipboard_has_text_ios(); }
  static neui_data_item_t NEUI_ABI c_read(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return neui_data_item_none;
    uint32_t id = s->_data_items.allocate();
    auto* item = s->_data_items.get(id);
    if (!item) return neui_data_item_none;
    if (!neui_detail::clipboard_read_item_ios(*item)) {
      s->_data_items.release(id);
      return neui_data_item_none;
    }
    return { id };
  }
  static neui_data_item_t NEUI_ABI c_create_item(neui_session_t session)
  {
    auto* s = get_session(session);
    return s ? neui_data_item_t{ s->_data_items.allocate() } : neui_data_item_none;
  }
  static void NEUI_ABI c_release(neui_session_t session, neui_data_item_t item)
  {
    auto* s = get_session(session);
    if (s) s->_data_items.release(item.id);
  }
  static int NEUI_ABI c_write(neui_session_t session, neui_data_item_t item)
  {
    auto* s = get_session(session);
    if (!s) return 0;
    auto* it = s->_data_items.get(item.id);
    return it && neui_detail::clipboard_write_item_ios(*it) ? 1 : 0;
  }
  static int NEUI_ABI c_item_set_format(neui_session_t session, neui_data_item_t item,
                                        const char* mime, const void* data, uint32_t length)
  {
    auto* s = get_session(session);
    if (!s || !mime) return 0;
    auto* it = s->_data_items.get(item.id);
    if (!it) return 0;
    it->set_format(mime, data, length);
    return 1;
  }
  static int NEUI_ABI c_item_get_format(neui_session_t session, neui_data_item_t item,
                                        const char* mime, void* buf, int buflen)
  {
    auto* s = get_session(session);
    if (!s || !mime) return -1;
    auto* it = s->_data_items.get(item.id);
    return it ? it->get_format(mime, buf, buflen) : -1;
  }
  static bool NEUI_ABI c_item_has_format(neui_session_t session, neui_data_item_t item, const char* mime)
  {
    auto* s = get_session(session);
    if (!s || !mime) return false;
    auto* it = s->_data_items.get(item.id);
    return it && it->has_format(mime);
  }
  static int NEUI_ABI c_item_set_format_callback(neui_session_t session, neui_data_item_t item,
                                                 const char* mime,
                                                 neui_data_provider_t provider, void* userdata)
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
    c_set_text, c_get_text, c_has_text,
    c_read, c_create_item, c_release,
    c_write,
    c_item_set_format, c_item_get_format, c_item_has_format,
    c_item_set_format_callback,
  };

  // -------------------------------------------------------------------------
  // Commands API. invoke_focused routes built-in editing commands to the
  // first responder (UIKit responder chain), defined in window.mm.

  static int NEUI_ABI cmd_invoke_focused(neui_session_t /*session*/, uint32_t cmd)
  {
    return invoke_focused_command_ios(cmd) ? 1 : 0;
  }
  static int NEUI_ABI cmd_invoke(neui_session_t /*session*/, neui_widget_t /*widget*/, uint32_t /*cmd*/)
  {
    // TODO(ios phase 2): widget-targeted command routing.
    return 0;
  }

  neui_commands_api_t commands_api = {
    NEUI_VERSION,
    cmd_invoke_focused, cmd_invoke,
  };

  // -------------------------------------------------------------------------
  // Scroll API (NEUI_API_SCROLL) - shared section-scroll math.

  static void section_external_commit_ios(WidgetData& sec, int nx, int ny)
  {
    if (!sec.section_scroll_state) return;
    auto& st = *sec.section_scroll_state;
    auto& L  = sec.section_last_layout;
    int max_x = st.content_w - L.body_w; if (max_x < 0) max_x = 0;
    int max_y = st.content_h - L.body_h; if (max_y < 0) max_y = 0;
    if (nx < 0) nx = 0; if (nx > max_x) nx = max_x;
    if (ny < 0) ny = 0; if (ny > max_y) ny = max_y;
    if (st.scroll_x == nx && st.scroll_y == ny) return;
    st.scroll_x = nx; st.scroll_y = ny;
    st.kin_v.raw_px = (double)ny; st.kin_v.last_commit_px = ny; st.kin_v.suppress_momentum = true;
    st.kin_h.raw_px = (double)nx; st.kin_h.last_commit_px = nx; st.kin_h.suppress_momentum = true;
    st.kinetic_over_v = st.kinetic_over_h = false;
    section_reposition_children_ios(sec);
    mark_widget_dirty_for_paint(sec);
    section_notify_scroll_changed_ios(sec);
  }

  static int NEUI_ABI scroll_set(neui_session_t session, neui_widget_t widget, int sx, int sy)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    auto& wd = s->_widgets[idx];
    if (!wd.section_scroll_state) return 0;
    section_external_commit_ios(wd, sx, sy);
    return 1;
  }
  static int NEUI_ABI scroll_get(neui_session_t session, neui_widget_t widget, int* ox, int* oy)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return 0;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return 0;
    auto& wd = s->_widgets[idx];
    if (!wd.section_scroll_state) return 0;
    if (ox) *ox = wd.section_scroll_state->scroll_x;
    if (oy) *oy = wd.section_scroll_state->scroll_y;
    return 1;
  }

  static uint32_t find_scrolling_section_ancestor_ios(Session* s, uint32_t widget_idx,
                                                      int& out_x, int& out_y)
  {
    out_x = out_y = 0;
    if (!s || !s->_widgets.exists(widget_idx)) return 0;
    auto& wd0 = s->_widgets[widget_idx];
    out_x = wd0.x; out_y = wd0.y;
    uint32_t cur = s->_widgets.get_parent(widget_idx);
    while (cur != 0 && cur != neui_detail::knone.id && s->_widgets.exists(cur)) {
      auto& cw = s->_widgets[cur];
      if (cw.section_scroll_state) return cur;
      out_x += cw.x; out_y += cw.y;
      cur = s->_widgets.get_parent(cur);
    }
    return 0;
  }

  void Session::ensure_widget_visible(uint32_t widget_idx)
  {
    using namespace neui_detail;
    if (!_widgets.exists(widget_idx)) return;
    auto& wd0 = _widgets[widget_idx];
    int rect_x = 0, rect_y = 0;
    uint32_t sec_idx = find_scrolling_section_ancestor_ios(this, widget_idx, rect_x, rect_y);
    if (sec_idx == 0) return;
    auto& sec = _widgets[sec_idx];
    auto& st  = *sec.section_scroll_state;
    auto& L   = sec.section_last_layout;
    int nx, ny;
    compute_ensure_visible(rect_x, rect_y, wd0.width, wd0.height,
                           L.body_w, L.body_h, st.content_w, st.content_h,
                           st.scroll_x, st.scroll_y, nx, ny);
    section_external_commit_ios(sec, nx, ny);
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
    scroll_set, scroll_get, scroll_ensure_visible,
  };

  // -------------------------------------------------------------------------
  // Tabs API (NEUI_API_TABS) - selection control over a TABVIEW. Tabs are the
  // TABVIEW's NEUI_W_TABPAGE children in creation order. Mirror of the macOS
  // native host's tabs_api.

  // Resolve `widget` to a TABVIEW WidgetData* (valid, this session, type
  // TABVIEW) or nullptr.
  static WidgetData* tabview_from_ios(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return nullptr;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return nullptr;
    auto& wd = s->_widgets[idx];
    if (!wd.type || strcmp(wd.type, NEUI_W_TABVIEW) != 0) return nullptr;
    return &wd;
  }

  static uint32_t NEUI_ABI tabs_count(neui_session_t session, neui_widget_t tabview)
  {
    WidgetData* tv = tabview_from_ios(session, tabview);
    if (!tv) return 0;
    std::vector<uint32_t> pages; tabview_collect_pages_ios(*tv, pages);
    return (uint32_t)pages.size();
  }

  static uint32_t NEUI_ABI tabs_get_selected(neui_session_t session, neui_widget_t tabview)
  {
    WidgetData* tv = tabview_from_ios(session, tabview);
    if (!tv) return NEUI_ITEM_NONE;
    std::vector<uint32_t> pages; tabview_collect_pages_ios(*tv, pages);
    if (pages.empty()) return NEUI_ITEM_NONE;
    int sel = tv->tab_selected;
    if (sel < 0) sel = 0;
    if (sel >= (int)pages.size()) sel = (int)pages.size() - 1;
    return (uint32_t)sel;
  }

  static void NEUI_ABI tabs_set_selected(neui_session_t session,
                                         neui_widget_t tabview, uint32_t index)
  {
    WidgetData* tv = tabview_from_ios(session, tabview);
    if (!tv) return;
    // Clamp huge / sentinel indices (e.g. NEUI_ITEM_NONE) to a representable
    // int so the cast doesn't wrap negative - per the documented "clamped to
    // [0, count)", an out-of-range index selects the LAST tab, not the first.
    int ni = index > 0x7fffffffu ? 0x7fffffff : (int)index;
    tabview_select_ios(*tv, ni);
  }

  static neui_widget_t NEUI_ABI tabs_get_page(neui_session_t session,
                                              neui_widget_t tabview, uint32_t index)
  {
    WidgetData* tv = tabview_from_ios(session, tabview);
    if (!tv) return widget_none;
    std::vector<uint32_t> pages; tabview_collect_pages_ios(*tv, pages);
    if (index >= pages.size()) return widget_none;
    return neui_widget_t{ tv->session->_widgets[pages[index]].widget_id };
  }

  static uint32_t NEUI_ABI tabs_get_index(neui_session_t session,
                                          neui_widget_t tabview, neui_widget_t page)
  {
    WidgetData* tv = tabview_from_ios(session, tabview);
    if (!tv) return NEUI_ITEM_NONE;
    std::vector<uint32_t> pages; tabview_collect_pages_ios(*tv, pages);
    for (uint32_t i = 0; i < pages.size(); ++i)
      if (tv->session->_widgets[pages[i]].widget_id == page.id)
        return i;
    return NEUI_ITEM_NONE;
  }

  neui_tabs_api_t tabs_api = {
    NEUI_VERSION,
    tabs_count, tabs_get_selected, tabs_set_selected, tabs_get_page, tabs_get_index,
  };

  // -------------------------------------------------------------------------
  // Asset API - delegates to the shared IOSAssetManager.

  static neui_asset_t pack_asset_ios(uint32_t session_id, uint32_t slot)
  { return { ((session_id & 0xffff) << 16) | (slot & 0xffff) }; }

  static neui_asset_t NEUI_ABI as_create_bitmap(neui_session_t session, uint32_t w_px,
                                                uint32_t h_px, const uint8_t* bgra, float scale)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_bitmap(w_px, h_px, bgra, scale);
    return slot ? pack_asset_ios(s->session_id(), slot) : asset_none;
  }
  static neui_asset_t NEUI_ABI as_create_from_file(neui_session_t session, const char* path)
  {
    auto* s = get_session(session);
    if (!s || !path) return asset_none;
    float scale = (float)UIScreen.mainScreen.scale;
    if (scale <= 0) scale = 1.0f;
    uint32_t slot = s->_asset_manager.allocate_from_file(path, scale);
    return slot ? pack_asset_ios(s->session_id(), slot) : asset_none;
  }
  static void NEUI_ABI as_destroy(neui_session_t session, neui_asset_t asset)
  {
    auto* s = get_session(session);
    if (!s || asset.id == asset_none.id) return;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    s->_asset_manager.release_slot(asset.id & 0xffff, neui_cg_backend::get_backend());
  }
  static bool NEUI_ABI as_get_size(neui_session_t session, neui_asset_t asset, float* ow, float* oh)
  {
    auto* s = get_session(session);
    if (!s || asset.id == asset_none.id) return false;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return false;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!e) return false;
    if (e->kind == NEUI_ASSET_KIND_COMPONENT) {
      if (ow) *ow = e->comp_w;
      if (oh) *oh = e->comp_h;
      return true;
    }
    if (e->scale <= 0.0f) return false;
    if (ow) *ow = (float)e->width_px / e->scale;
    if (oh) *oh = (float)e->height_px / e->scale;
    return true;
  }
  static neui_asset_kind_t NEUI_ABI as_get_kind(neui_session_t session, neui_asset_t asset)
  {
    auto* s = get_session(session);
    if (!s || asset.id == asset_none.id) return NEUI_ASSET_KIND_NONE;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return NEUI_ASSET_KIND_NONE;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    return e ? e->kind : NEUI_ASSET_KIND_NONE;
  }
  static neui_asset_t NEUI_ABI as_create_compound(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_compound();
    return slot ? pack_asset_ios(s->session_id(), slot) : asset_none;
  }
  static neui_asset_t NEUI_ABI as_create_behavior(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_behavior();
    return slot ? pack_asset_ios(s->session_id(), slot) : asset_none;
  }
  static neui_asset_t NEUI_ABI as_create_filter(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_filter();
    return slot ? pack_asset_ios(s->session_id(), slot) : asset_none;
  }
  static void NEUI_ABI as_apply_filter(neui_session_t session,
                                       neui_asset_t surface, neui_asset_t filter)
  {
    auto* s = get_session(session);
    if (!s || surface.id == asset_none.id || filter.id == asset_none.id) return;
    if (((surface.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    if (((filter.id  >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    s->_asset_manager.apply_filter(surface.id & 0xffff, filter.id & 0xffff,
                                   neui_cg_backend::get_backend());
  }
  static neui_asset_t NEUI_ABI as_create_surface(neui_session_t session, float wl, float hl, float scale)
  {
    auto* s = get_session(session);
    if (!s || wl <= 0.0f || hl <= 0.0f) return asset_none;
    if (scale <= 0.0f) scale = 1.0f;
    uint32_t w_px = (uint32_t)(wl * scale + 0.5f), h_px = (uint32_t)(hl * scale + 0.5f);
    uint32_t slot = s->_asset_manager.allocate_surface(w_px, h_px, scale, neui_cg_backend::get_backend());
    return slot ? pack_asset_ios(s->session_id(), slot) : asset_none;
  }
  static void NEUI_ABI as_paint_surface(neui_session_t session, neui_asset_t surface,
                                        uint32_t clear_argb, neui_surface_paint_fn fn, void* user)
  {
    auto* s = get_session(session);
    if (!s || surface.id == asset_none.id) return;
    if (((surface.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    s->_asset_manager.paint_surface(surface.id & 0xffff, clear_argb, fn, user,
                                    neui_cg_backend::get_backend(), s,
                                    &ios_painter_draw_asset_thunk);
  }
  static void NEUI_ABI as_surface_blur(neui_session_t session, neui_asset_t surface,
                                       float sigma_x, float sigma_y)
  {
    auto* s = get_session(session);
    if (!s || surface.id == asset_none.id) return;
    if (((surface.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    s->_asset_manager.blur_surface(surface.id & 0xffff, sigma_x, sigma_y,
                                   neui_cg_backend::get_backend());
  }
  static void NEUI_ABI as_surface_drop_shadow(neui_session_t session, neui_asset_t surface,
                                              float dx, float dy, float sigma,
                                              uint32_t shadow_argb)
  {
    auto* s = get_session(session);
    if (!s || surface.id == asset_none.id) return;
    if (((surface.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    s->_asset_manager.drop_shadow_surface(surface.id & 0xffff, dx, dy, sigma, shadow_argb,
                                          neui_cg_backend::get_backend());
  }
#define NEUI_IOS_SURF_GUARD \
    auto* s = get_session(session); \
    if (!s || surface.id == asset_none.id) return; \
    if (((surface.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return
  static void NEUI_ABI as_surface_inner_shadow(neui_session_t session, neui_asset_t surface,
        float dx, float dy, float sigma, uint32_t shadow_argb)
  { NEUI_IOS_SURF_GUARD; s->_asset_manager.inner_shadow_surface(surface.id & 0xffff, dx, dy, sigma,
                                                                shadow_argb, neui_cg_backend::get_backend()); }
  static void NEUI_ABI as_surface_glow(neui_session_t session, neui_asset_t surface,
        float sigma, uint32_t glow_argb)
  { NEUI_IOS_SURF_GUARD; s->_asset_manager.glow_surface(surface.id & 0xffff, sigma, glow_argb,
                                                        neui_cg_backend::get_backend()); }
  static void NEUI_ABI as_surface_tint(neui_session_t session, neui_asset_t surface, uint32_t argb)
  { NEUI_IOS_SURF_GUARD; s->_asset_manager.tint_surface(surface.id & 0xffff, argb,
                                                        neui_cg_backend::get_backend()); }
  static void NEUI_ABI as_surface_desaturate(neui_session_t session, neui_asset_t surface, float amount)
  { NEUI_IOS_SURF_GUARD; s->_asset_manager.desaturate_surface(surface.id & 0xffff, amount,
                                                              neui_cg_backend::get_backend()); }
  static void NEUI_ABI as_surface_elevation(neui_session_t session, neui_asset_t surface, float level)
  { NEUI_IOS_SURF_GUARD; s->_asset_manager.elevation_surface(surface.id & 0xffff, level,
                                                             neui_cg_backend::get_backend()); }
  static void NEUI_ABI as_surface_bevel(neui_session_t session, neui_asset_t surface,
        float dx, float dy, float sigma, uint32_t light_argb, uint32_t dark_argb)
  { NEUI_IOS_SURF_GUARD; s->_asset_manager.bevel_surface(surface.id & 0xffff, dx, dy, sigma,
                                                         light_argb, dark_argb, neui_cg_backend::get_backend()); }
#undef NEUI_IOS_SURF_GUARD

  static neui_asset_t NEUI_ABI as_create_font(neui_session_t session,
                                              const uint8_t* data, uint32_t len)
  {
    auto* s = get_session(session);
    if (!s || !data || len == 0) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_font(data, len, neui_cg_backend::get_backend());
    return slot ? pack_asset_ios(s->session_id(), slot) : asset_none;
  }
  static neui_asset_t NEUI_ABI as_create_font_from_file(neui_session_t session, const char* path)
  {
    auto* s = get_session(session);
    if (!s || !path) return asset_none;
    uint32_t slot = s->_asset_manager.allocate_font_from_file(path, neui_cg_backend::get_backend());
    return slot ? pack_asset_ios(s->session_id(), slot) : asset_none;
  }
  static uint32_t NEUI_ABI as_get_font_family(neui_session_t session, neui_asset_t font,
                                              char* out_buf, uint32_t cap)
  {
    auto* s = get_session(session);
    if (!s || font.id == asset_none.id) return 0;
    if (((font.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return 0;
    return s->_asset_manager.get_font_family(font.id & 0xffff, out_buf, cap);
  }

  // Asset / compound / behavior tables (compound_api / behavior_api defined
  // later in this TU; asset_api just below). Forward-declared so the component
  // thunks can hand all three to build_component.
  extern neui_asset_api_t    asset_api;
  extern neui_compound_api_t compound_api;
  extern neui_behavior_api_t behavior_api;

  static void release_built_component_ios(neui_session_t session,
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
    if (!built.ok) { release_built_component_ios(session, built); return asset_none; }
    uint32_t slot = s->_asset_manager.allocate_component(built);
    if (slot == 0) { release_built_component_ios(session, built); return asset_none; }
    return pack_asset_ios(s->session_id(), slot);
  }

  static neui_asset_t NEUI_ABI as_create_component_from_file(
      neui_session_t session, const char* path_utf8,
      const neui_component_env_t* env)
  {
    auto* s = get_session(session);
    if (!s || !path_utf8) return asset_none;
    // Client resource provider first, then the file (shared read-or-ask helper).
    std::string data;
    if (!s->_asset_manager.resource_provider().read_bytes(
            NEUI_RESOURCE_KIND_COMPONENT, path_utf8, data))
      return asset_none;
    neui_component_env_t local{};
    const neui_component_env_t* use_env = env;
    static thread_local std::string base_keep;
    if (!env || !env->base_dir) {
      std::string p = path_utf8;
      size_t cut = p.find_last_of("/\\");
      base_keep = (cut == std::string::npos) ? std::string() : p.substr(0, cut);
      if (env) local = *env;
      local.base_dir = base_keep.c_str();
      use_env = &local;
    }
    return as_create_component_from_string(session, data.c_str(),
                                           static_cast<uint32_t>(data.size()), use_env);
  }

  static uint32_t NEUI_ABI as_component_param_count(neui_session_t session,
                                                    neui_asset_t component)
  {
    auto* s = get_session(session);
    if (!s || component.id == asset_none.id) return 0;
    if (((component.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return 0;
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
    if (((component.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return false;
    auto* e = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPONENT) return false;
    if (index >= e->comp_params.size()) return false;
    const auto& p = e->comp_params[index];
    out->key = p.key.c_str(); out->label = p.label.c_str();
    out->min = p.min; out->max = p.max; out->def = p.def;
    return true;
  }

  static uint32_t NEUI_ABI as_serialize_component(neui_session_t session,
                                                  neui_asset_t component,
                                                  char* out_buf, uint32_t cap,
                                                  int indent)
  {
    auto* s = get_session(session);
    if (!s || component.id == asset_none.id) return 0;
    if (((component.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return 0;
    auto* e = s->_asset_manager.get_slot(component.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPONENT) return 0;
    neui_detail::ComponentSerializeInput in;
    in.name = &e->comp_name; in.width = e->comp_w; in.height = e->comp_h;
    in.params = &e->comp_params;
    in.asset_names = &e->comp_asset_names;
    in.asset_handle_names = &e->comp_asset_handle_names;
    in.asset_frame_layouts = &e->comp_asset_frame_layouts;
    auto* cce = s->_asset_manager.get_slot(e->comp_compound.id & 0xffff);
    auto* bbe = s->_asset_manager.get_slot(e->comp_behavior.id & 0xffff);
    in.compound = (cce && cce->compound) ? cce->compound.get() : nullptr;
    in.behavior = (bbe && bbe->behavior) ? bbe->behavior.get() : nullptr;
    std::string json = neui_detail::serialize_component(in, indent);
    uint32_t full = static_cast<uint32_t>(json.size());
    if (out_buf && cap > 0) {
      uint32_t n = (full > cap - 1) ? cap - 1 : full;
      if (n) std::memcpy(out_buf, json.data(), n);
      out_buf[n] = '\0';
    }
    return full;
  }

  static bool NEUI_ABI as_set_frame_layout(neui_session_t session, neui_asset_t asset,
                                            uint32_t cols, uint32_t rows, uint32_t gutter_px)
  {
    auto* s = get_session(session);
    if (!s || asset.id == asset_none.id) return false;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return false;
    return s->_asset_manager.set_frame_layout(asset.id & 0xffff, cols, rows, gutter_px);
  }
  static neui_asset_t NEUI_ABI as_create_filmstrip_from_file(
      neui_session_t session, const char* path, uint32_t frame_count,
      neui_filmstrip_orientation_t orientation)
  {
    auto* s = get_session(session);
    if (!s || !path) return asset_none;
    float scale = (float)UIScreen.mainScreen.scale;
    if (scale <= 0) scale = 1.0f;
    uint32_t slot = s->_asset_manager.allocate_filmstrip_from_file(
        path, scale, frame_count, orientation == NEUI_FILMSTRIP_HORIZONTAL,
        neui_cg_backend::get_backend());
    return slot ? pack_asset_ios(s->session_id(), slot) : asset_none;
  }
  static uint32_t NEUI_ABI as_get_frame_count(neui_session_t session, neui_asset_t asset)
  {
    auto* s = get_session(session);
    if (!s || asset.id == asset_none.id) return 0;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return 0;
    return s->_asset_manager.frame_count(asset.id & 0xffff);
  }

  neui_asset_api_t asset_api = {
    NEUI_VERSION,
    as_create_bitmap, as_create_from_file, as_destroy,
    as_get_size, as_get_kind,
    as_create_compound, as_create_behavior,
    as_create_surface, as_paint_surface,
    as_create_font, as_create_font_from_file, as_get_font_family,
    as_create_component_from_string,
    as_create_component_from_file,
    as_component_param_count,
    as_component_param_at,
    as_serialize_component,
    as_set_frame_layout,
    as_create_filmstrip_from_file,
    as_get_frame_count,
    as_surface_blur,
    as_surface_drop_shadow,
    as_create_filter,
    as_apply_filter,
    as_surface_inner_shadow,
    as_surface_glow,
    as_surface_tint,
    as_surface_desaturate,
    as_surface_elevation,
    as_surface_bevel,
  };

  // -------------------------------------------------------------------------
  // Compound API - shared compound.h mutators + invalidate-attached walk.

  static neui_detail::CompoundAsset* resolve_compound_ios(neui_session_t session,
                                                          neui_asset_t asset, Session*& out)
  {
    out = nullptr;
    auto* s = get_session(session);
    if (!s || asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPOUND || !e->compound) return nullptr;
    out = s;
    return e->compound.get();
  }
  static neui_detail::CompoundLayer* resolve_layer_ios(neui_session_t session, neui_asset_t asset,
                                                       neui_compound_layer_t layer, Session*& out)
  {
    auto* ca = resolve_compound_ios(session, asset, out);
    if (!ca) return nullptr;
    if (neui_detail::compound_layer_asset_slot(layer) != (asset.id & 0xffff)) { out = nullptr; return nullptr; }
    return neui_detail::compound_get_layer(*ca, neui_detail::compound_layer_slot(layer));
  }

  static neui_compound_layer_t NEUI_ABI co_add_layer(neui_session_t session, neui_asset_t asset,
                                                     neui_compound_layer_kind_t kind, int z)
  {
    Session* s = nullptr;
    auto* ca = resolve_compound_ios(session, asset, s);
    if (!ca) return compound_layer_none;
    uint32_t slot = neui_detail::compound_add_layer(*ca, kind, z);
    s->invalidate_widgets_with_compound(asset.id);
    return neui_detail::pack_compound_layer(asset.id & 0xffff, slot);
  }
  static neui_compound_layer_t NEUI_ABI co_add_child_layer(neui_session_t session, neui_asset_t asset,
                                                           neui_compound_layer_t parent,
                                                           neui_compound_layer_kind_t kind, int z)
  {
    Session* s = nullptr;
    auto* ca = resolve_compound_ios(session, asset, s);
    if (!ca) return compound_layer_none;
    if (neui_detail::compound_layer_asset_slot(parent) != (asset.id & 0xffff))
      return compound_layer_none;
    uint32_t slot = neui_detail::compound_add_child_layer(
      *ca, neui_detail::compound_layer_slot(parent), kind, z);
    if (slot == UINT32_MAX) return compound_layer_none;
    s->invalidate_widgets_with_compound(asset.id);
    return neui_detail::pack_compound_layer(asset.id & 0xffff, slot);
  }
  static void NEUI_ABI co_remove_layer(neui_session_t session, neui_asset_t asset, neui_compound_layer_t layer)
  {
    Session* s = nullptr;
    auto* ca = resolve_compound_ios(session, asset, s);
    if (!ca || neui_detail::compound_layer_asset_slot(layer) != (asset.id & 0xffff)) return;
    neui_detail::compound_remove_layer(*ca, neui_detail::compound_layer_slot(layer));
    s->invalidate_widgets_with_compound(asset.id);
  }
  static void NEUI_ABI co_clear(neui_session_t session, neui_asset_t asset)
  {
    Session* s = nullptr;
    auto* ca = resolve_compound_ios(session, asset, s);
    if (!ca) return;
    neui_detail::compound_clear(*ca);
    s->invalidate_widgets_with_compound(asset.id);
  }
  static void NEUI_ABI co_set_z(neui_session_t session, neui_asset_t asset, neui_compound_layer_t layer, int z)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_ios(session, asset, layer, s);
    if (!L) return;
    L->z = z; s->invalidate_widgets_with_compound(asset.id);
  }
  static void NEUI_ABI co_set_anchor(neui_session_t session, neui_asset_t asset, neui_compound_layer_t layer,
                                     neui_anchor_t pa, neui_anchor_t sa)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_ios(session, asset, layer, s);
    if (!L) return;
    L->parent_anchor = pa; L->self_anchor = sa; s->invalidate_widgets_with_compound(asset.id);
  }
  static void NEUI_ABI co_set_int(neui_session_t session, neui_asset_t asset, neui_compound_layer_t layer,
                                  const char* prop, int value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_ios(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_int(*L, prop, value); s->invalidate_widgets_with_compound(asset.id);
  }
  static void NEUI_ABI co_set_float(neui_session_t session, neui_asset_t asset, neui_compound_layer_t layer,
                                    const char* prop, float value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_ios(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_float(*L, prop, value); s->invalidate_widgets_with_compound(asset.id);
  }
  static void NEUI_ABI co_set_string(neui_session_t session, neui_asset_t asset, neui_compound_layer_t layer,
                                     const char* prop, const char* value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_ios(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_string(*L, prop, value); s->invalidate_widgets_with_compound(asset.id);
  }
  static void NEUI_ABI co_set_asset(neui_session_t session, neui_asset_t asset, neui_compound_layer_t layer,
                                    const char* prop, neui_asset_t value)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_ios(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_set_asset(*L, prop, value); s->invalidate_widgets_with_compound(asset.id);
  }
  static void NEUI_ABI co_bind(neui_session_t session, neui_asset_t asset, neui_compound_layer_t layer,
                               const char* prop, const char* attr_key, float scale, float offset)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_ios(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_bind(*L, prop, attr_key, scale, offset); s->invalidate_widgets_with_compound(asset.id);
  }
  static void NEUI_ABI co_bind_asset(neui_session_t session, neui_asset_t asset, neui_compound_layer_t layer,
                                     const char* prop, const char* attr_key)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_ios(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_bind_asset(*L, prop, attr_key); s->invalidate_widgets_with_compound(asset.id);
  }
  static void NEUI_ABI co_unbind(neui_session_t session, neui_asset_t asset, neui_compound_layer_t layer,
                                 const char* prop)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_ios(session, asset, layer, s);
    if (!L || !prop) return;
    neui_detail::apply_unbind(*L, prop); s->invalidate_widgets_with_compound(asset.id);
  }
  static void NEUI_ABI co_set_path(neui_session_t session, neui_asset_t asset, neui_compound_layer_t layer,
                                   const neui_path_cmd_t* cmds, uint32_t count)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_ios(session, asset, layer, s);
    if (!L) return;
    neui_detail::apply_set_path(*L, cmds, count); s->invalidate_widgets_with_compound(asset.id);
  }

  static void NEUI_ABI co_set_gradient(neui_session_t session, neui_asset_t asset,
                                       neui_compound_layer_t layer, const neui_gradient_t* grad)
  {
    Session* s = nullptr;
    auto* L = resolve_layer_ios(session, asset, layer, s);
    if (!L) return;
    neui_detail::apply_set_gradient(*L, grad); s->invalidate_widgets_with_compound(asset.id);
  }

  neui_compound_api_t compound_api = {
    NEUI_VERSION,
    co_add_layer, co_remove_layer, co_clear, co_set_z, co_set_anchor,
    co_set_int, co_set_float, co_set_string, co_set_asset,
    co_bind, co_bind_asset, co_unbind, co_set_path, co_set_gradient,
    co_add_child_layer,
  };

  // -------------------------------------------------------------------------
  // Behavior API - shared behavior.h mutators.

  static neui_detail::BehaviorAsset* resolve_behavior_ios(neui_session_t session,
                                                          neui_asset_t asset, Session*& out)
  {
    out = nullptr;
    auto* s = get_session(session);
    if (!s || asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_BEHAVIOR || !e->behavior) return nullptr;
    out = s;
    return e->behavior.get();
  }
  static neui_detail::BehaviorHandler* resolve_behavior_handler_ios(neui_session_t session,
        neui_asset_t asset, neui_behavior_handler_t handler, Session*& out)
  {
    auto* ba = resolve_behavior_ios(session, asset, out);
    if (!ba) return nullptr;
    if (neui_detail::behavior_handler_asset_slot(handler) != (asset.id & 0xffff)) { out = nullptr; return nullptr; }
    return neui_detail::behavior_get_handler(*ba, neui_detail::behavior_handler_slot(handler));
  }

  static neui_behavior_handler_t NEUI_ABI be_add_handler(neui_session_t session, neui_asset_t asset,
                                                         neui_behavior_kind_t kind)
  {
    Session* s = nullptr;
    auto* ba = resolve_behavior_ios(session, asset, s);
    if (!ba) return behavior_handler_none;
    uint32_t slot = neui_detail::behavior_add_handler(*ba, kind);
    return neui_detail::pack_behavior_handler(asset.id & 0xffff, slot);
  }
  static void NEUI_ABI be_remove_handler(neui_session_t session, neui_asset_t asset, neui_behavior_handler_t handler)
  {
    Session* s = nullptr;
    auto* ba = resolve_behavior_ios(session, asset, s);
    if (!ba || neui_detail::behavior_handler_asset_slot(handler) != (asset.id & 0xffff)) return;
    neui_detail::behavior_remove_handler(*ba, neui_detail::behavior_handler_slot(handler));
  }
  static void NEUI_ABI be_clear(neui_session_t session, neui_asset_t asset)
  {
    Session* s = nullptr;
    auto* ba = resolve_behavior_ios(session, asset, s);
    if (ba) neui_detail::behavior_clear(*ba);
  }
  static void NEUI_ABI be_set_int(neui_session_t session, neui_asset_t asset,
                                  neui_behavior_handler_t handler, const char* prop, int value)
  {
    Session* s = nullptr;
    auto* H = resolve_behavior_handler_ios(session, asset, handler, s);
    if (H && prop) neui_detail::apply_behavior_set_int(*H, prop, value);
  }
  static void NEUI_ABI be_set_float(neui_session_t session, neui_asset_t asset,
                                    neui_behavior_handler_t handler, const char* prop, float value)
  {
    Session* s = nullptr;
    auto* H = resolve_behavior_handler_ios(session, asset, handler, s);
    if (H && prop) neui_detail::apply_behavior_set_float(*H, prop, value);
  }
  static void NEUI_ABI be_set_string(neui_session_t session, neui_asset_t asset,
                                     neui_behavior_handler_t handler, const char* prop, const char* value)
  {
    Session* s = nullptr;
    auto* H = resolve_behavior_handler_ios(session, asset, handler, s);
    if (H && prop) neui_detail::apply_behavior_set_string(*H, prop, value);
  }

  neui_behavior_api_t behavior_api = {
    NEUI_VERSION,
    be_add_handler, be_remove_handler, be_clear,
    be_set_int, be_set_float, be_set_string,
  };

  // -------------------------------------------------------------------------
  // Filter API (NEUI_API_FILTER) - shared FilterAsset graph, applied to a
  // SURFACE via assets->apply_filter.
  static neui_detail::FilterAsset* resolve_filter_ios(neui_session_t session,
                                                      neui_asset_t asset, Session*& out)
  {
    out = nullptr;
    auto* s = get_session(session);
    if (!s || asset.id == asset_none.id) return nullptr;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return nullptr;
    auto* e = s->_asset_manager.get_slot(asset.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_FILTER || !e->filter) return nullptr;
    out = s;
    return e->filter.get();
  }
  static neui_detail::FilterPrimitive* resolve_filter_prim_ios(neui_session_t session,
        neui_asset_t asset, neui_filter_prim_t prim, Session*& out)
  {
    auto* fa = resolve_filter_ios(session, asset, out);
    if (!fa) return nullptr;
    if (neui_detail::filter_prim_asset_slot(prim) != (asset.id & 0xffff)) return nullptr;
    return neui_detail::filter_get_prim(*fa, neui_detail::filter_prim_slot(prim));
  }
  static neui_filter_prim_t NEUI_ABI fi_add_primitive(neui_session_t session,
        neui_asset_t asset, neui_filter_prim_kind_t kind)
  {
    Session* s = nullptr; auto* fa = resolve_filter_ios(session, asset, s);
    if (!fa) return filter_prim_none;
    return neui_detail::pack_filter_prim(asset.id & 0xffff,
                                         neui_detail::filter_add_primitive(*fa, kind));
  }
  static void NEUI_ABI fi_remove_primitive(neui_session_t session, neui_asset_t asset,
        neui_filter_prim_t prim)
  {
    Session* s = nullptr; auto* fa = resolve_filter_ios(session, asset, s);
    if (!fa || neui_detail::filter_prim_asset_slot(prim) != (asset.id & 0xffff)) return;
    neui_detail::filter_remove_primitive(*fa, neui_detail::filter_prim_slot(prim));
  }
  static void NEUI_ABI fi_clear(neui_session_t session, neui_asset_t asset)
  {
    Session* s = nullptr; auto* fa = resolve_filter_ios(session, asset, s);
    if (fa) neui_detail::filter_clear(*fa);
  }
  static void NEUI_ABI fi_set_input(neui_session_t session, neui_asset_t asset,
        neui_filter_prim_t prim, int slot, const char* source)
  {
    Session* s = nullptr; auto* P = resolve_filter_prim_ios(session, asset, prim, s);
    if (P) neui_detail::apply_filter_set_input(*P, slot, source);
  }
  static void NEUI_ABI fi_set_result(neui_session_t session, neui_asset_t asset,
        neui_filter_prim_t prim, const char* name)
  {
    Session* s = nullptr; auto* P = resolve_filter_prim_ios(session, asset, prim, s);
    if (P) neui_detail::apply_filter_set_result(*P, name);
  }
  static void NEUI_ABI fi_set_region(neui_session_t session, neui_asset_t asset,
        neui_filter_prim_t prim, float x, float y, float w, float h)
  {
    Session* s = nullptr; auto* P = resolve_filter_prim_ios(session, asset, prim, s);
    if (P) neui_detail::apply_filter_set_region(*P, x, y, w, h);
  }
  static void NEUI_ABI fi_set_int(neui_session_t session, neui_asset_t asset,
        neui_filter_prim_t prim, const char* prop, int value)
  {
    Session* s = nullptr; auto* P = resolve_filter_prim_ios(session, asset, prim, s);
    if (P && prop) neui_detail::apply_filter_set_int(*P, prop, value);
  }
  static void NEUI_ABI fi_set_float(neui_session_t session, neui_asset_t asset,
        neui_filter_prim_t prim, const char* prop, float value)
  {
    Session* s = nullptr; auto* P = resolve_filter_prim_ios(session, asset, prim, s);
    if (P && prop) neui_detail::apply_filter_set_float(*P, prop, value);
  }
  static void NEUI_ABI fi_set_string(neui_session_t session, neui_asset_t asset,
        neui_filter_prim_t prim, const char* prop, const char* value)
  {
    Session* s = nullptr; auto* P = resolve_filter_prim_ios(session, asset, prim, s);
    if (P && prop) neui_detail::apply_filter_set_string(*P, prop, value);
  }
  static void NEUI_ABI fi_set_floats(neui_session_t session, neui_asset_t asset,
        neui_filter_prim_t prim, const char* prop, const float* values, uint32_t count)
  {
    Session* s = nullptr; auto* P = resolve_filter_prim_ios(session, asset, prim, s);
    if (P && prop) neui_detail::apply_filter_set_floats(*P, prop, values, count);
  }
  static void NEUI_ABI fi_merge_add_input(neui_session_t session, neui_asset_t asset,
        neui_filter_prim_t prim, const char* source)
  {
    Session* s = nullptr; auto* P = resolve_filter_prim_ios(session, asset, prim, s);
    if (P) neui_detail::apply_filter_merge_add_input(*P, source);
  }
  neui_filter_api_t filter_api = {
    NEUI_VERSION,
    fi_add_primitive, fi_remove_primitive, fi_clear,
    fi_set_input, fi_set_result, fi_set_region,
    fi_set_int, fi_set_float, fi_set_string, fi_set_floats,
    fi_merge_add_input,
  };

  // -------------------------------------------------------------------------
  // DnD API. Drop-target state round-trips + drives a real UIDropInteraction on
  // the content view (window.mm). Drag source is gesture-driven via a
  // UIDragInteraction: a widget is draggable when it carries a DRAG_SOURCE
  // behavior asset (the interaction delegate resolves it). The blocking
  // begin_drag() contract does NOT hold on iOS (a UIDrag can only originate from
  // the system drag gesture), so begin_drag returns NONE immediately - the
  // documented iOS divergence (see include/neui/d/dnd.h).

  static void NEUI_ABI d_set_drop_target(neui_session_t session, neui_widget_t widget, bool enable)
  {
    auto* s = get_session(session);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (s->_widgets.exists(idx)) s->_widgets[idx].drop_target = enable;
  }
  static bool NEUI_ABI d_get_drop_target(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session(session);
    if (!s) return false;
    uint32_t idx = WidgetToIndex(widget);
    return s->_widgets.exists(idx) ? s->_widgets[idx].drop_target : false;
  }
  static void NEUI_ABI d_set_accepted_formats(neui_session_t session, neui_widget_t widget,
                                              const char* const* mimes, int count)
  {
    auto* s = get_session(session);
    if (!s) return;
    uint32_t idx = WidgetToIndex(widget);
    if (!s->_widgets.exists(idx)) return;
    auto& w = s->_widgets[idx];
    w.accepted_mimes.clear();
    if (mimes && count > 0) {
      w.accepted_mimes.reserve((size_t)count);
      for (int i = 0; i < count; ++i) if (mimes[i]) w.accepted_mimes.emplace_back(mimes[i]);
    }
  }
  static void NEUI_ABI d_accept(neui_session_t session, neui_dnd_action_t action)
  {
    auto* s = get_session(session);
    if (s && s->_in_dnd_dispatch) s->_last_accepted_action = (uint32_t)action;
  }
  // iOS divergence: drags are gesture-driven (UIDragInteraction + a DRAG_SOURCE
  // behavior asset), never programmatic; begin_drag returns NONE immediately.
  static neui_dnd_action_t NEUI_ABI d_begin_drag(neui_session_t, neui_widget_t,
                                                 neui_data_item_t, uint32_t)
  { return NEUI_DND_ACTION_NONE; }
  static neui_dnd_action_t NEUI_ABI d_begin_drag_with_preview(neui_session_t, neui_widget_t,
                                                              neui_data_item_t, uint32_t,
                                                              const neui_drag_preview_t*)
  { return NEUI_DND_ACTION_NONE; }

  neui_dnd_api_t dnd_api = {
    NEUI_VERSION,
    d_set_drop_target, d_get_drop_target, d_set_accepted_formats,
    d_accept, d_begin_drag, d_begin_drag_with_preview,
  };

  // -------------------------------------------------------------------------
  // Notify API - toast (shared overlay paint + CADisplayLink in window.mm) +
  // message box (UIAlertController via message_box_ios.h).

  static WidgetData* notify_frame_ios(neui_session_t session, neui_widget_t frame)
  {
    auto* s = get_session_for_widget(session, frame);
    if (!s) return nullptr;
    uint32_t i = WidgetToIndex(frame);
    if (!s->_widgets.exists(i)) return nullptr;
    auto& wd = s->_widgets[i];
    if (!wd.isroot) return nullptr;
    return &wd;
  }
  static void NEUI_ABI notify_toast(neui_session_t session, neui_widget_t frame, const char* text)
  {
    WidgetData* wd = notify_frame_ios(session, frame);
    if (wd && text) notify_toast_ios(*wd, text);
  }
  static int NEUI_ABI notify_message_box(neui_session_t session, neui_widget_t frame,
                                         const char* text, const char* caption, uint32_t flags)
  {
    WidgetData* wd = notify_frame_ios(session, frame);
    if (!wd) return 0;
    return notify_message_box_ios(*wd, text, caption, flags);
  }

  neui_notify_api_t notify_api = {
    NEUI_VERSION,
    notify_toast, notify_message_box,
  };

  // -------------------------------------------------------------------------
  // Grid API - full GridModel wiring (mirror of hosts/macos/widgets.mm). The
  // 43 methods are platform-neutral model calls over the shared grid_model.h;
  // the native painted GRID view in window.mm reuses widget_paint_grid.h +
  // scroll_kinetics.h verbatim for paint + touch input. P3 (cell editing /
  // sort header clicks) is wired here too - it falls out of the shared model
  // for free; the in-place EDIT overlay's keyboard path is hardware-keyboard
  // only on iOS (no on-screen-keyboard plumbing in v1, see window.mm
  // grid_painted_char_ios + the TODO there).

  // window.mm grid cell-edit dispatch helpers (need the painted view + events).
  bool grid_try_begin_edit_ios(WidgetData& wd, int row, int col);
  bool grid_commit_edit_ios(WidgetData& wd);
  void grid_cancel_edit_ios(WidgetData& wd);

  static WidgetData* resolve_grid_ios(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session_for_widget(session, widget);
    if (!s) return nullptr;
    uint32_t i = WidgetToIndex(widget);
    if (!s->_widgets.exists(i)) return nullptr;
    auto& wd = s->_widgets[i];
    if (!wd.type || strcmp(wd.type, NEUI_W_GRID) != 0) return nullptr;
    return &wd;
  }

  static neui_detail::GridModel& ensure_grid_model_ios_api(WidgetData& wd)
  {
    if (!wd.grid_model)
      wd.grid_model = std::make_unique<neui_detail::GridModel>();
    return *wd.grid_model;
  }

  static void grid_invalidate_ios(WidgetData* wd)
  {
    if (wd) mark_widget_dirty_for_paint(*wd);
  }

  // Viewport from the painted view's current bounds (logical points).
  static neui_detail::GridViewport grid_viewport_ios_api(WidgetData& wd)
  {
    auto& m   = ensure_grid_model_ios_api(wd);
    auto  cfg = neui_detail::grid_read_config(wd.attrs.get());
    return neui_detail::grid_compute_viewport(m, wd.width, wd.height,
                                              cfg.row_h, cfg.header_h);
  }

  static int NEUI_ABI gr_add_column(neui_session_t session, neui_widget_t widget,
                                      const char* header, int width_logical)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd) return -1;
    auto& m = ensure_grid_model_ios_api(*wd);
    neui_detail::GridColumn c;
    c.header = header ? header : "";
    c.width  = (width_logical > 0) ? width_logical : neui_detail::GRID_DEFAULT_NEW_COLUMN_W;
    m.columns.push_back(std::move(c));
    neui_detail::grid_resize_rows_to_columns(m, (int)m.columns.size());
    grid_invalidate_ios(wd);
    return (int)m.columns.size() - 1;
  }

  static int NEUI_ABI gr_get_column_count(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_ios(session, widget);
    return wd && wd->grid_model ? (int)wd->grid_model->columns.size() : 0;
  }

  static void NEUI_ABI gr_set_column_width(neui_session_t session, neui_widget_t widget,
                                             int col, int width_logical)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    int min_w = neui_detail::grid_column_min_width(m, col, cfg.col_min_w_def);
    if (width_logical < min_w) width_logical = min_w;
    m.columns[(size_t)col].width = width_logical;
    grid_invalidate_ios(wd);
  }

  static int NEUI_ABI gr_get_column_width(neui_session_t session, neui_widget_t widget, int col)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return 0;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return 0;
    return m.columns[(size_t)col].width;
  }

  static void NEUI_ABI gr_set_column_min_width(neui_session_t session, neui_widget_t widget,
                                                  int col, int min_w)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].min_width = min_w;
    if (m.columns[(size_t)col].width < min_w)
      m.columns[(size_t)col].width = min_w;
    grid_invalidate_ios(wd);
  }

  static void NEUI_ABI gr_set_column_align(neui_session_t session, neui_widget_t widget,
                                             int col, const char* align)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].align = neui_detail::grid_parse_align(align);
    grid_invalidate_ios(wd);
  }

  static void NEUI_ABI gr_set_column_header(neui_session_t session, neui_widget_t widget,
                                              int col, const char* text)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].header = text ? text : "";
    grid_invalidate_ios(wd);
  }

  static int NEUI_ABI gr_get_column_header(neui_session_t session, neui_widget_t widget,
                                             int col, char* buf, int buflen)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return -1;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return -1;
    const std::string& h = m.columns[(size_t)col].header;
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
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns.erase(m.columns.begin() + col);
    for (auto& r : m.rows)
      if (col < (int)r.cells.size()) r.cells.erase(r.cells.begin() + col);
    std::unordered_map<uint64_t, neui_detail::GridCellOverride> remap;
    for (auto& kv : m.cell_overrides) {
      int r = (int)(kv.first >> 32);
      int c = (int)(kv.first & 0xFFFFFFFF);
      if (c == col) continue;
      int nc = (c > col) ? c - 1 : c;
      remap[neui_detail::grid_cell_key(r, nc)] = kv.second;
    }
    m.cell_overrides = std::move(remap);
    if (m.selected_col >= (int)m.columns.size())
      m.selected_col = (int)m.columns.size() - 1;
    neui_detail::grid_sort_on_column_removed(m, col);
    grid_invalidate_ios(wd);
  }

  static void NEUI_ABI gr_clear_columns(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_ios_api(*wd);
    m.columns.clear();
    m.rows.clear();
    m.cell_overrides.clear();
    m.selected_row = -1;
    m.selected_col = -1;
    m.scroll_offset_x = 0;
    m.scroll_offset_y = 0;
    m.scroll_px_offset = 0;
    m.sort_stack.clear();
    m.display_order.clear();
    m.logical_to_visual.clear();
    m.sort_dirty = false;
    grid_invalidate_ios(wd);
  }

  static int NEUI_ABI gr_add_row(neui_session_t session, neui_widget_t widget,
                                   const char* const* values_utf8)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd) return -1;
    auto& m = ensure_grid_model_ios_api(*wd);
    neui_detail::GridRow row;
    row.cells.resize(m.columns.size());
    if (values_utf8) {
      for (size_t i = 0; i < m.columns.size() && values_utf8[i]; ++i)
        row.cells[i] = values_utf8[i];
    }
    m.rows.push_back(std::move(row));
    m.sort_dirty = true;
    grid_invalidate_ios(wd);
    return (int)m.rows.size() - 1;
  }

  static int NEUI_ABI gr_get_row_count(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_ios(session, widget);
    return wd && wd->grid_model ? (int)wd->grid_model->rows.size() : 0;
  }

  static void NEUI_ABI gr_remove_row(neui_session_t session, neui_widget_t widget, int row)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return;
    m.rows.erase(m.rows.begin() + row);
    std::unordered_map<uint64_t, neui_detail::GridCellOverride> remap;
    for (auto& kv : m.cell_overrides) {
      int r = (int)(kv.first >> 32);
      int c = (int)(kv.first & 0xFFFFFFFF);
      if (r == row) continue;
      int nr = (r > row) ? r - 1 : r;
      remap[neui_detail::grid_cell_key(nr, c)] = kv.second;
    }
    m.cell_overrides = std::move(remap);
    if (m.selected_row >= (int)m.rows.size())
      m.selected_row = (int)m.rows.size() - 1;
    m.sort_dirty = true;
    grid_invalidate_ios(wd);
  }

  static void NEUI_ABI gr_clear_rows(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    m.rows.clear();
    m.cell_overrides.clear();
    m.selected_row = -1;
    m.scroll_offset_y = 0;
    m.scroll_px_offset = 0;
    m.display_order.clear();
    m.logical_to_visual.clear();
    m.sort_dirty = false;
    grid_invalidate_ios(wd);
  }

  static void NEUI_ABI gr_set_cell_text(neui_session_t session, neui_widget_t widget,
                                          int row, int col, const char* utf8)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return;
    if (col < 0 || col >= (int)m.columns.size()) return;
    auto& r = m.rows[(size_t)row];
    if ((int)r.cells.size() <= col) r.cells.resize((size_t)col + 1);
    r.cells[(size_t)col] = utf8 ? utf8 : "";
    m.sort_dirty = true;
    grid_invalidate_ios(wd);
  }

  static int NEUI_ABI gr_get_cell_text(neui_session_t session, neui_widget_t widget,
                                         int row, int col, char* buf, int buflen)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return -1;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return -1;
    if (col < 0 || col >= (int)m.columns.size()) return -1;
    const auto& r = m.rows[(size_t)row];
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
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return;
    if (col < 0 || col >= (int)m.columns.size()) return;
    if (argb == 0) {
      auto* ov = neui_detail::grid_find_override(m, row, col);
      if (ov) {
        ov->has_color = false;
        ov->color     = 0;
        neui_detail::grid_prune_override(m, row, col);
      }
    } else {
      auto& ov = neui_detail::grid_ensure_override(m, row, col);
      ov.color     = argb;
      ov.has_color = true;
    }
    grid_invalidate_ios(wd);
  }

  static void NEUI_ABI gr_set_cell_enabled(neui_session_t session, neui_widget_t widget,
                                             int row, int col, bool enabled)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (row < 0 || row >= (int)m.rows.size()) return;
    if (col < 0 || col >= (int)m.columns.size()) return;
    auto& ov = neui_detail::grid_ensure_override(m, row, col);
    ov.enabled     = enabled;
    ov.has_enabled = true;
    if (enabled && !ov.has_color) {
      ov.has_enabled = false;
      neui_detail::grid_prune_override(m, row, col);
    }
    grid_invalidate_ios(wd);
  }

  static void NEUI_ABI gr_clear_cell_overrides(neui_session_t session, neui_widget_t widget,
                                                  int row, int col)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return;
    wd->grid_model->cell_overrides.erase(neui_detail::grid_cell_key(row, col));
    grid_invalidate_ios(wd);
  }

  static void NEUI_ABI gr_set_selected_row(neui_session_t session, neui_widget_t widget, int row)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_ios_api(*wd);
    int n = (int)m.rows.size();
    if (row < -1)  row = -1;
    if (row >= n)  row = n - 1;
    m.selected_row = row;
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    if (cfg.cell_focus && m.selected_col < 0 && !m.columns.empty())
      m.selected_col = 0;
    if (row >= 0) {
      m.scroll_px_offset = 0;   // programmatic selection snaps to row alignment
      auto vp = grid_viewport_ios_api(*wd);
      neui_detail::grid_ensure_row_visible(m, vp, cfg.row_h, row);
    }
    grid_invalidate_ios(wd);
  }

  static int NEUI_ABI gr_get_selected_row(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_ios(session, widget);
    return wd && wd->grid_model ? wd->grid_model->selected_row : -1;
  }

  static void NEUI_ABI gr_set_selected_cell(neui_session_t session, neui_widget_t widget,
                                              int row, int col)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_ios_api(*wd);
    int n_rows = (int)m.rows.size();
    int n_cols = (int)m.columns.size();
    if (row < -1)       row = -1;
    if (row >= n_rows)  row = n_rows - 1;
    if (col < -1)       col = -1;
    if (col >= n_cols)  col = n_cols - 1;
    m.selected_row = row;
    m.selected_col = col;
    if (row >= 0 && col >= 0) {
      m.scroll_px_offset = 0;   // programmatic selection snaps to row alignment
      auto cfg = neui_detail::grid_read_config(wd->attrs.get());
      auto vp  = grid_viewport_ios_api(*wd);
      neui_detail::grid_ensure_cell_visible(m, vp, cfg.row_h, row, col);
    }
    grid_invalidate_ios(wd);
  }

  static void NEUI_ABI gr_get_selected_cell(neui_session_t session, neui_widget_t widget,
                                              int* out_row, int* out_col)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (out_row) *out_row = (wd && wd->grid_model) ? wd->grid_model->selected_row : -1;
    if (out_col) {
      if (!wd || !wd->grid_model) { *out_col = -1; return; }
      auto cfg = neui_detail::grid_read_config(wd->attrs.get());
      *out_col = cfg.cell_focus ? wd->grid_model->selected_col : -1;
    }
  }

  static void NEUI_ABI gr_ensure_row_visible(neui_session_t session, neui_widget_t widget, int row)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_ios_api(*wd);
    m.scroll_px_offset = 0;   // snap to row alignment
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    auto vp  = grid_viewport_ios_api(*wd);
    neui_detail::grid_ensure_row_visible(m, vp, cfg.row_h, row);
    grid_invalidate_ios(wd);
  }

  static void NEUI_ABI gr_ensure_cell_visible(neui_session_t session, neui_widget_t widget,
                                                int row, int col)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_ios_api(*wd);
    m.scroll_px_offset = 0;   // snap to row alignment
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    auto vp  = grid_viewport_ios_api(*wd);
    neui_detail::grid_ensure_cell_visible(m, vp, cfg.row_h, row, col);
    grid_invalidate_ios(wd);
  }

  static void NEUI_ABI gr_set_scroll_x(neui_session_t session, neui_widget_t widget, int x)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_ios_api(*wd);
    m.scroll_offset_x = x;
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    auto vp  = grid_viewport_ios_api(*wd);
    neui_detail::grid_clamp_scroll(m, vp, cfg.row_h);
    grid_invalidate_ios(wd);
  }

  static int NEUI_ABI gr_get_scroll_x(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_ios(session, widget);
    return wd && wd->grid_model ? wd->grid_model->scroll_offset_x : 0;
  }

  static int NEUI_ABI gr_hit_test(neui_session_t session, neui_widget_t widget,
                                    int lx, int ly, int* out_row, int* out_col)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (out_row) *out_row = -1;
    if (out_col) *out_col = -1;
    if (!wd || !wd->grid_model) return 0;
    auto& m = *wd->grid_model;
    auto cfg = neui_detail::grid_read_config(wd->attrs.get());
    auto vp  = grid_viewport_ios_api(*wd);
    neui_detail::grid_ensure_sort_clean(m);
    auto hit = neui_detail::grid_hit_test(m, vp, cfg.row_h,
                                           wd->width, wd->height, lx, ly);
    if (hit.region != neui_detail::GridHitRegion::Cell) return 0;
    if (out_row) *out_row = hit.row;
    if (out_col) *out_col = hit.col;
    return 1;
  }

  // -------- Sort API ----------------------------------------------------

  static void NEUI_ABI gr_set_column_sortable(neui_session_t session, neui_widget_t widget,
                                                int col, bool sortable)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].sortable = sortable;
  }

  static void NEUI_ABI gr_set_column_sort_kind(neui_session_t session, neui_widget_t widget,
                                                 int col, neui_grid_sort_kind_t kind)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].sort_kind = kind;
    if (neui_detail::grid_sort_stack_find(m, col) >= 0) {
      m.sort_dirty = true;
      grid_invalidate_ios(wd);
    }
  }

  static void NEUI_ABI gr_set_sort(neui_session_t session, neui_widget_t widget,
                                     int col, neui_grid_sort_dir_t dir)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_ios_api(*wd);
    neui_detail::grid_set_sort(m, col, dir);
    grid_invalidate_ios(wd);
  }

  static void NEUI_ABI gr_add_sort(neui_session_t session, neui_widget_t widget,
                                     int col, neui_grid_sort_dir_t dir)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_ios_api(*wd);
    neui_detail::grid_add_sort(m, col, dir);
    grid_invalidate_ios(wd);
  }

  static void NEUI_ABI gr_clear_sort(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_ios_api(*wd);
    neui_detail::grid_clear_sort(m);
    grid_invalidate_ios(wd);
  }

  static int NEUI_ABI gr_get_sort_count(neui_session_t session, neui_widget_t widget)
  {
    auto* wd = resolve_grid_ios(session, widget);
    return (wd && wd->grid_model) ? (int)wd->grid_model->sort_stack.size() : 0;
  }

  static void NEUI_ABI gr_get_sort_level(neui_session_t session, neui_widget_t widget,
                                           int level, int* out_col,
                                           neui_grid_sort_dir_t* out_dir)
  {
    if (out_col) *out_col = -1;
    if (out_dir) *out_dir = NEUI_GRID_SORT_NONE;
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return;
    auto& m = *wd->grid_model;
    if (level < 0 || level >= (int)m.sort_stack.size()) return;
    if (out_col) *out_col = m.sort_stack[(size_t)level].col;
    if (out_dir) *out_dir = m.sort_stack[(size_t)level].dir;
  }

  static int NEUI_ABI gr_logical_to_visual_row(neui_session_t session, neui_widget_t widget,
                                                  int logical_row)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return -1;
    auto& m = *wd->grid_model;
    if (logical_row < 0 || logical_row >= (int)m.rows.size()) return -1;
    neui_detail::grid_ensure_sort_clean(m);
    return neui_detail::grid_logical_to_visual(m, logical_row);
  }

  static int NEUI_ABI gr_visual_to_logical_row(neui_session_t session, neui_widget_t widget,
                                                  int visual_row)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return -1;
    auto& m = *wd->grid_model;
    if (visual_row < 0 || visual_row >= (int)m.rows.size()) return -1;
    neui_detail::grid_ensure_sort_clean(m);
    return neui_detail::grid_visual_to_logical(m, visual_row);
  }

  // -------- Cell editing API --------------------------------------------

  static void NEUI_ABI gr_set_column_editable(neui_session_t session, neui_widget_t widget,
                                                int col, bool editable)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd) return;
    auto& m = ensure_grid_model_ios_api(*wd);
    if (col < 0 || col >= (int)m.columns.size()) return;
    m.columns[(size_t)col].editable = editable;
    if (!editable && m.edit.active && m.edit.col == col)
      grid_cancel_edit_ios(*wd);
  }

  static bool NEUI_ABI gr_get_column_editable(neui_session_t session, neui_widget_t widget,
                                                int col)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model) return false;
    auto& m = *wd->grid_model;
    if (col < 0 || col >= (int)m.columns.size()) return false;
    return m.columns[(size_t)col].editable;
  }

  static void NEUI_ABI gr_begin_cell_edit(neui_session_t session, neui_widget_t widget,
                                           int row, int col)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd) return;
    if (grid_try_begin_edit_ios(*wd, row, col) && wd->native_control) {
      // Pull keyboard focus to the painted view so a hardware keyboard routes
      // typing here (no on-screen keyboard for the painted grid editor in v1).
      UIView* v = (__bridge UIView*)wd->native_control;
      if ([v canBecomeFirstResponder]) [v becomeFirstResponder];
    }
  }

  static void NEUI_ABI gr_end_cell_edit(neui_session_t session, neui_widget_t widget,
                                         bool commit)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model || !wd->grid_model->edit.active) return;
    if (commit) (void)grid_commit_edit_ios(*wd);
    else        grid_cancel_edit_ios(*wd);
  }

  static bool NEUI_ABI gr_is_editing_cell(neui_session_t session, neui_widget_t widget,
                                            int* out_row, int* out_col)
  {
    auto* wd = resolve_grid_ios(session, widget);
    if (!wd || !wd->grid_model || !wd->grid_model->edit.active) {
      if (out_row) *out_row = -1;
      if (out_col) *out_col = -1;
      return false;
    }
    auto& m = *wd->grid_model;
    if (out_row) *out_row = m.edit.row;
    if (out_col) *out_col = m.edit.col;
    return true;
  }

  neui_grid_api_t grid_api = {
    NEUI_VERSION,
    gr_add_column, gr_get_column_count, gr_set_column_width, gr_get_column_width,
    gr_set_column_min_width, gr_set_column_align, gr_set_column_header, gr_get_column_header,
    gr_remove_column, gr_clear_columns,
    gr_add_row, gr_get_row_count, gr_remove_row, gr_clear_rows,
    gr_set_cell_text, gr_get_cell_text, gr_set_cell_color, gr_set_cell_enabled,
    gr_clear_cell_overrides,
    gr_set_selected_row, gr_get_selected_row,
    gr_set_selected_cell, gr_get_selected_cell,
    gr_ensure_row_visible, gr_ensure_cell_visible,
    gr_set_scroll_x, gr_get_scroll_x,
    gr_hit_test,
    gr_set_column_sortable, gr_set_column_sort_kind,
    gr_set_sort, gr_add_sort, gr_clear_sort,
    gr_get_sort_count, gr_get_sort_level,
    gr_logical_to_visual_row, gr_visual_to_logical_row,
    gr_set_column_editable, gr_get_column_editable,
    gr_begin_cell_edit, gr_end_cell_edit, gr_is_editing_cell,
  };

} // namespace ios_host
