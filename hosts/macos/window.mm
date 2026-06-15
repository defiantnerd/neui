// Native macOS host - NSApp lifecycle, NSWindow plumbing, NEUINativeWindowDelegate,
// NEUINativeControlTarget (target-action sink), NEUINativeContentView (flipped
// container), and Session::widget_show dispatch.
//
// Step 4 of plans/native-macos-host.md adds LABEL + BUTTON. Per-step
// extension below the switch in widget_show.

#import <AppKit/AppKit.h>
#import <objc/runtime.h>

#include "host.h"
#include "checkbox_image.h"
#include "../shared/macos/image_loader_macos.h"
#include "../shared/macos/keys_macos.h"
#include "../shared/macos/theme_provider_macos.h"
#include "../shared/macos/clipboard_macos.h"
#include "../shared/macos/modal_pump_macos.h"
#include "../shared/macos/dnd_helpers_macos.h"
#include "../shared/macos/window_helpers_macos.h"
#include "../shared/dnd_modifier_suggest.h"
#include "../shared/widget_paint_knob.h"
#include "../shared/widget_paint_compound.h"
#include "../shared/widget_paint_section.h"
#include "../shared/widget_paint_tabview.h"
#include "../shared/widget_paint_grid.h"
#include "../shared/painter.h"
#include "../../backends/cg/cg_backend.h"

#include <cstring>

// NEUINativeWindowDelegate carries the (Session, widget_index) pair for its
// window. NEUINativeContentView's <NSDraggingDestination> methods reach
// through window.delegate to dispatch DnD, so the full ivar layout must be
// visible before that @implementation (further down).
@interface NEUINativeWindowDelegate : NSObject<NSWindowDelegate>
{
@public
  macos_host::Session* session;
  uint32_t             widget_index;
  bool                 is_appwindow;
  bool                 handled_close;
  __weak NSWindow*     sheet_owner;
  bool                 sheet_active;
}
@end

@class NEUINativeControlTarget;
@class NEUINativeContentView;
@class NEUINativeTextDelegate;
@class NEUINativeListSource;
@class NEUINativeOutlineSource;
@class NEUINativeMenuTarget;
@class NEUINativePaintedView;

// Forward declaration of the widget-by-id lookup (defined further down
// inside namespace macos_host) - NEUINativePaintedView's @implementation
// references it before the definition appears.
namespace macos_host {
  WidgetData* widget_for_id(uint32_t widget_id, Session** out_session = nullptr);

  // A TABPAGE is a chip-less SECTION: it reuses ALL the section_* machinery
  // (scroll state, inner body view, child clipping, kinetics). Every site
  // that early-outs on `strcmp(type, NEUI_W_SECTION)` instead tests this so
  // pages get the same body/scroll/clip treatment as sections. The chip is
  // suppressed by section_effective_text_macos / _align_macos returning
  // ""/"none" for a TABPAGE, so band_h == 0 and the body fills the whole rect.
  inline bool is_section_like(const char* type)
  {
    return type && (!strcmp(type, NEUI_W_SECTION) ||
                    !strcmp(type, NEUI_W_TABPAGE));
  }
  // The header-chip text the SECTION paint band should use. Empty for a
  // TABPAGE (its `text` is the tab label drawn by the parent TABVIEW, not a
  // section header chip). Mirror of xpl TabPageWidget::section_header_text.
  inline const char* section_effective_text_macos(const WidgetData& wd)
  {
    if (wd.type && !strcmp(wd.type, NEUI_W_TABPAGE)) return "";
    return wd.text.c_str();
  }
  // The chip alignment the SECTION band should use. "none" for a TABPAGE so
  // band_h collapses to 0 + the body fills the rect. Mirror of xpl
  // TabPageWidget::section_header_align.
  inline const char* section_effective_align_macos(const WidgetData& wd)
  {
    if (wd.type && !strcmp(wd.type, NEUI_W_TABPAGE)) return "none";
    return wd.attrs ? wd.attrs->get_string(NEUI_ATTR_ALIGN_TEXT) : nullptr;
  }

  // TABVIEW helpers (defined further down). The painted view's drawRect: /
  // mouseDown: reference them before the definitions appear.
  void tabview_collect_pages_macos(WidgetData& tv, std::vector<uint32_t>& out);
  void tabview_select_macos(WidgetData& tv, int new_index);
  void tabview_apply_page_geometry_macos(WidgetData& tv);

  // GRID in-place editor character input (defined further down inside the
  // namespace). The keyDown: implementation references it before the
  // definition appears.
  void grid_painted_char_macos(WidgetData& wd, uint32_t cp);

  // Scrolling-SECTION helpers (defined further down). The painted view's
  // drawRect: / mouseDown: / mouseDragged: / mouseUp: / scrollWheel:
  // reference them before the definitions appear.
  void section_refresh_scroll_state_macos(WidgetData& wd);
  void section_compute_layout_macos(WidgetData& wd);
  void section_reposition_children_macos(WidgetData& sec);
  void section_apply_layout_changes_macos(WidgetData& sec);
  bool section_bounce_step_macos(WidgetData& wd);
  void section_notify_scroll_changed_macos(WidgetData& wd);
  void parent_scroll_offset_macos(Session* sess, uint32_t widget_index,
                                    int& out_x, int& out_y);

  // Resolve the body-fill ARGB for a SECTION (NEUI_ATTR_BACKGROUND override or
  // the direction-aware SECTION_BG_LIFT default). Shared by the section paint
  // branch + the inner body view's drawRect.
  uint32_t section_resolve_bg_argb(WidgetData& wd);

  // Inner body view lifecycle for a scrolling SECTION. Mirror of the win32
  // host's section_create_body_hwnd_w32 / _destroy / _reparent_children. The
  // NEUISectionBodyView referenced here is declared just below alongside
  // NEUINativePaintedView.
  void section_create_body_view_macos(WidgetData& sec);
  void section_destroy_body_view_macos(WidgetData& sec);
  void section_reparent_children_macos(WidgetData& sec, bool to_body);
  void section_ensure_body_view_macos(WidgetData& sec);
  NSView* section_child_container_macos(const WidgetData& sec);

  // Painter draw_asset thunk - mirror of
  // hosts/win32/widgets.cpp::w32_painter_draw_asset_thunk. Resolves the
  // neui_asset_t through the session's MacOSAssetManager, lazy-uploads a
  // per-(asset, ctx) CGImage on first use, then calls backend->draw_bitmap.
  void NEUI_ABI macos_painter_draw_asset_thunk(void* host_token,
                                                 neui_render_backend_t* backend,
                                                 neui_render_ctx_t ctx,
                                                 neui_asset_t asset,
                                                 float x, float y,
                                                 float w, float h,
                                                 uint32_t tint);

  // Blocking popup menu. Builds an NSMenu from the NULL-terminated UTF-8
  // `items` ("-" = separator), presents it at `(x, y)` in `anchor`'s
  // (flipped) coordinate space, and returns the 1-based index of the picked
  // item (separators consume an index, matching the win32 host) or 0 on
  // dismiss. Defined after the view; the KNOB right-click handler calls it.
  int run_popup_menu_macos(NSView* anchor, int x, int y, const char* const* items);

  // Defined in widgets.mm. Routes a built-in command (NEUI_CMD_*) to the key
  // window's first responder; returns true if consumed. The menu-pick router
  // calls it before falling back to TREE_ITEM_ACTIVATED.
  bool invoke_focused_command_macos(uint32_t cmd);
}

// ---------------------------------------------------------------------------
// Behavior plumbing for CUSTOMDRAW.
//
// CUSTOMDRAW widgets carry an optional NEUI_ASSET_KIND_BEHAVIOR via
// widgets->set_asset. When attached, the shared dispatch
// (hosts/shared/behavior_runtime.h) interprets mouse / key / wheel events
// and writes target attrs in the widget's AttrBag, firing
// NEUI_EVENT_ATTR_CHANGED on user-driven mutations. These helpers wrap
// the host callbacks (invalidate, emit, popup) so the dispatch is a
// single call from the view's input methods.

namespace macos_host {

  static neui_detail::BehaviorAsset*
  resolve_widget_behavior_macos(WidgetData& wd, Session* sess)
  {
    if (!sess) return nullptr;
    neui_asset_t a = wd.behavior_asset;
    if (a.id == asset_none.id) return nullptr;
    if (((a.id >> 16) & 0xffff) != (sess->session_id() & 0xffff)) return nullptr;
    auto* e = sess->_asset_manager.get_slot(a.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_BEHAVIOR || !e->behavior) return nullptr;
    return e->behavior.get();
  }

  // Forward-decl: defined later in this TU after the painted view + popup
  // helper are in scope.
  static int macos_behavior_popup_menu(void* host_data, int local_x, int local_y,
                                         const char* const* items);

  static void macos_behavior_invalidate(void* host_data)
  {
    auto* wd = static_cast<WidgetData*>(host_data);
    if (!wd || !wd->native_control) return;
    NSView* v = (__bridge NSView*)wd->native_control;
    [v setNeedsDisplay:YES];
  }

  // Invalidate the painted view if its compound has state-filtered layers.
  // Called from NEUINativePaintedView's hover / press transitions so the
  // compound repaints when NEUI_LAYER_STATE_* changes.
  static void macos_invalidate_if_state_filtered_compound(WidgetData* wd)
  {
    if (!wd || !wd->session) return;
    if (wd->compound_asset.id == asset_none.id) return;
    if (((wd->compound_asset.id >> 16) & 0xffff)
          != (wd->session->session_id() & 0xffff)) return;
    auto* e = wd->session->_asset_manager.get_slot(wd->compound_asset.id & 0xffff);
    if (!e || e->kind != NEUI_ASSET_KIND_COMPOUND || !e->compound) return;
    if (!neui_detail::compound_has_state_filters(*e->compound)) return;
    if (!wd->native_control) return;
    NSView* v = (__bridge NSView*)wd->native_control;
    [v setNeedsDisplay:YES];
  }

  static void macos_behavior_emit_attr_changed(void* host_data,
                                                 const char* attr_key, float value)
  {
    auto* wd = static_cast<WidgetData*>(host_data);
    if (!wd || !wd->session || !wd->emit_events) return;
    neui_event_t ev = {};
    ev.type                 = NEUI_EVENT_ATTR_CHANGED;
    ev.data.attr.widget.id  = wd->widget_id;
    ev.data.attr.attr_key   = attr_key;
    ev.data.attr.value      = value;
    wd->session->dispatch_event(&ev);
  }

  // Defined in widgets.mm where d_begin_drag_with_preview is in scope.
  // Forwarded here so DRAG_SOURCE handlers can fire begin_drag through
  // the same path the public dnd_api uses.
  uint32_t macos_behavior_begin_drag(void* host_data,
                                       neui_data_item_t item,
                                       uint32_t allowed_actions,
                                       uint32_t preview_image,
                                       int hot_x, int hot_y);

  static neui_detail::BehaviorDispatchCtx make_behavior_ctx_macos(WidgetData& wd)
  {
    neui_detail::BehaviorDispatchCtx ctx{};
    ctx.bag      = &neui_detail::ensure_attrs(wd.attrs);
    ctx.widget_w = static_cast<float>(wd.width);
    ctx.widget_h = static_cast<float>(wd.height);
    ctx.host_data         = &wd;
    ctx.invalidate        = &macos_behavior_invalidate;
    ctx.emit_attr_changed = &macos_behavior_emit_attr_changed;
    ctx.popup_menu        = &macos_behavior_popup_menu;
    ctx.begin_drag        = &macos_behavior_begin_drag;
    return ctx;
  }

  // Dispatch an already-built neui_event_t through the widget's behavior
  // asset. Returns true if a handler consumed the event.
  static bool dispatch_behavior_mouse(uint32_t widget_id, neui_event_t* ev,
                                        float local_x, float local_y)
  {
    Session* sess = nullptr;
    auto* wd = widget_for_id(widget_id, &sess);
    if (!wd || !sess) return false;
    auto* ba = resolve_widget_behavior_macos(*wd, sess);
    if (!ba) return false;
    if (!wd->behavior_rt)
      wd->behavior_rt = std::make_unique<neui_detail::BehaviorRuntime>();
    auto ctx = make_behavior_ctx_macos(*wd);
    return neui_detail::behavior_dispatch_mouse(*ba, *wd->behavior_rt, ctx,
                                                  ev, local_x, local_y);
  }

  static bool dispatch_behavior_key(uint32_t widget_id, uint32_t keycode,
                                      uint32_t modifiers)
  {
    Session* sess = nullptr;
    auto* wd = widget_for_id(widget_id, &sess);
    if (!wd || !sess) return false;
    auto* ba = resolve_widget_behavior_macos(*wd, sess);
    if (!ba) return false;
    if (!wd->behavior_rt)
      wd->behavior_rt = std::make_unique<neui_detail::BehaviorRuntime>();
    auto ctx = make_behavior_ctx_macos(*wd);
    return neui_detail::behavior_dispatch_key(*ba, *wd->behavior_rt, ctx,
                                                keycode, modifiers);
  }

  static int macos_behavior_popup_menu(void* host_data, int local_x, int local_y,
                                         const char* const* items)
  {
    auto* wd = static_cast<WidgetData*>(host_data);
    if (!wd || !wd->native_control || !items) return 0;
    NSView* anchor = (__bridge NSView*)wd->native_control;
    return run_popup_menu_macos(anchor, local_x, local_y, items);
  }

  // ------------------------------------------------------------------------
  // GRID (NEUI_W_GRID) input dispatch. Direct translation of
  // hosts/win32/widgets.cpp::painted_msg_grid_w32. The visual + model live
  // in hosts/shared/grid_model.h + widget_paint_grid.h; this only carries
  // the macOS glue (event -> shared model mutation -> repaint + events).
  //
  // The win32 host runs the whole thing from one WndProc switch; AppKit
  // splits mouse events across mouseDown:/mouseDragged:/mouseUp:/mouseMoved:
  // /scrollWheel:/keyDown:, so we funnel each through grid_painted_msg_macos
  // with a small kind tag. macOS coordinates are already logical points (no
  // phys_to_log conversion); a held-button move arrives as GridMsg::Drag.

  static neui_detail::GridModel& ensure_grid_model_macos(WidgetData& wd)
  {
    if (!wd.grid_model)
      wd.grid_model = std::make_unique<neui_detail::GridModel>();
    return *wd.grid_model;
  }

  static void grid_repaint_macos(WidgetData& wd)
  {
    if (wd.native_control)
      [(__bridge NSView*)wd.native_control setNeedsDisplay:YES];
  }

  // Widget-local logical size from the painted view's bounds.
  static void grid_widget_size_macos(WidgetData& wd, int* out_w, int* out_h)
  {
    *out_w = 0; *out_h = 0;
    if (!wd.native_control) return;
    NSSize sz = ((__bridge NSView*)wd.native_control).bounds.size;
    *out_w = (int)sz.width;
    *out_h = (int)sz.height;
  }

  static neui_detail::GridViewport grid_viewport_macos(WidgetData& wd)
  {
    auto& m   = ensure_grid_model_macos(wd);
    auto  cfg = neui_detail::grid_read_config(wd.attrs.get());
    int   lw = 0, lh = 0;
    grid_widget_size_macos(wd, &lw, &lh);
    return neui_detail::grid_compute_viewport(m, lw, lh, cfg.row_h, cfg.header_h);
  }

  // -------- Event dispatch helpers (mirror the _w32 versions) ------------

  static bool grid_fire_row_selected_macos(WidgetData& wd, int row)
  {
    if (!wd.session) return false;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_ROW_SELECTED;
    ev.data.grid_row.widget.id = wd.widget_id;
    ev.data.grid_row.row       = row;
    return wd.session->dispatch_event(&ev);
  }

  static bool grid_fire_cell_selected_macos(WidgetData& wd, int row, int col)
  {
    if (!wd.session) return false;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_CELL_SELECTED;
    ev.data.grid_cell.widget.id = wd.widget_id;
    ev.data.grid_cell.row       = row;
    ev.data.grid_cell.col       = col;
    return wd.session->dispatch_event(&ev);
  }

  static bool grid_fire_cell_clicked_macos(WidgetData& wd, int row, int col)
  {
    if (!wd.session) return false;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_CELL_CLICKED;
    ev.data.grid_cell.widget.id = wd.widget_id;
    ev.data.grid_cell.row       = row;
    ev.data.grid_cell.col       = col;
    return wd.session->dispatch_event(&ev);
  }

  static void grid_fire_row_activated_macos(WidgetData& wd, int row)
  {
    if (!wd.session) return;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_ROW_ACTIVATED;
    ev.data.grid_row.widget.id = wd.widget_id;
    ev.data.grid_row.row       = row;
    wd.session->dispatch_event(&ev);
  }

  static void grid_fire_column_resized_macos(WidgetData& wd, int col,
                                               int old_w, int new_w)
  {
    if (!wd.session) return;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_COLUMN_RESIZED;
    ev.data.grid_column_resize.widget.id = wd.widget_id;
    ev.data.grid_column_resize.col       = col;
    ev.data.grid_column_resize.old_width = old_w;
    ev.data.grid_column_resize.new_width = new_w;
    wd.session->dispatch_event(&ev);
  }

  // Fire NEUI_EVENT_SCROLL_CHANGED iff the section's committed offset
  // moved since the last notification. Called from every commit path
  // (wheel kinetic + stepped, scrollbar drag, bounce tick, programmatic
  // scroll API). Sentinel INT32_MIN ensures the first notify fires.
  void section_notify_scroll_changed_macos(WidgetData& wd)
  {
    if (!wd.session || !wd.section_scroll_state) return;
    auto& st = *wd.section_scroll_state;
    if (st.scroll_x == st.last_notified_x &&
        st.scroll_y == st.last_notified_y) return;
    st.last_notified_x = st.scroll_x;
    st.last_notified_y = st.scroll_y;
    neui_event_t ev{};
    ev.type                  = NEUI_EVENT_SCROLL_CHANGED;
    ev.data.scroll.widget.id = wd.widget_id;
    ev.data.scroll.scroll_x  = st.scroll_x;
    ev.data.scroll.scroll_y  = st.scroll_y;
    wd.session->dispatch_event(&ev);
  }

  static void grid_fire_sort_changed_macos(WidgetData& wd, int col,
                                              neui_grid_sort_dir_t dir)
  {
    if (!wd.session) return;
    neui_event_t ev{};
    ev.type = NEUI_EVENT_GRID_SORT_CHANGED;
    ev.data.grid_sort.widget.id = wd.widget_id;
    ev.data.grid_sort.col       = col;
    ev.data.grid_sort.dir       = (int)dir;
    wd.session->dispatch_event(&ev);
  }

  // Dispatch ladder run after a body-cell click: always update selection,
  // then ROW_SELECTED -> (cell_focus ? CELL_SELECTED) -> CELL_CLICKED, each
  // only firing if the prior wasn't consumed.
  static void grid_click_ladder_macos(WidgetData& wd, int row, int col)
  {
    auto& m   = ensure_grid_model_macos(wd);
    auto  cfg = neui_detail::grid_read_config(wd.attrs.get());
    m.selected_row = row;
    if (cfg.cell_focus) m.selected_col = col;
    if (grid_fire_row_selected_macos(wd, row)) return;
    if (cfg.cell_focus) {
      if (grid_fire_cell_selected_macos(wd, row, col)) return;
    }
    grid_fire_cell_clicked_macos(wd, row, col);
  }

  // ---- Cell-edit dispatch helpers (macOS) --------------------------------

  static void grid_fire_cell_edit_event_macos(WidgetData& wd, neui_event_type_t t,
                                                int row, int col)
  {
    if (!wd.session) return;
    neui_event_t ev{};
    ev.type = t;
    ev.data.grid_cell.widget.id = wd.widget_id;
    ev.data.grid_cell.row       = row;
    ev.data.grid_cell.col       = col;
    wd.session->dispatch_event(&ev);
  }

  bool grid_try_begin_edit_macos(WidgetData& wd, int row, int col)
  {
    auto& m = ensure_grid_model_macos(wd);
    if (m.edit.active) return false;
    auto cfg = neui_detail::grid_read_config(wd.attrs.get());
    if (!neui_detail::grid_cell_edit_allowed(m, row, col, cfg.cell_focus))
      return false;
    neui_detail::grid_begin_edit(m, row, col);
    grid_repaint_macos(wd);
    grid_fire_cell_edit_event_macos(wd, NEUI_EVENT_GRID_CELL_EDIT_BEGIN,
                                      row, col);
    return true;
  }

  bool grid_commit_edit_macos(WidgetData& wd)
  {
    auto& m = ensure_grid_model_macos(wd);
    if (!m.edit.active) return false;
    int row = m.edit.row;
    int col = m.edit.col;
    auto* client = wd.session ? wd.session->_grid_client : nullptr;
    const std::string proposed = m.edit.te.text;
    if (client && client->validate_cell) {
      neui_widget_t w{}; w.id = wd.widget_id;
      if (!client->validate_cell(wd.session->get_token(), w, row, col,
                                   proposed.c_str())) {
        return false;
      }
    }
    (void)neui_detail::grid_end_edit(m);
    auto& r = m.rows[(size_t)row];
    if ((int)r.cells.size() <= col) r.cells.resize((size_t)col + 1);
    r.cells[(size_t)col] = proposed;
    m.sort_dirty = true;
    grid_repaint_macos(wd);
    grid_fire_cell_edit_event_macos(wd, NEUI_EVENT_GRID_CELL_CHANGED, row, col);
    return true;
  }

  void grid_cancel_edit_macos(WidgetData& wd)
  {
    auto& m = ensure_grid_model_macos(wd);
    if (!m.edit.active) return;
    int row = m.edit.row;
    int col = m.edit.col;
    (void)neui_detail::grid_end_edit(m);
    grid_repaint_macos(wd);
    grid_fire_cell_edit_event_macos(wd, NEUI_EVENT_GRID_CELL_EDIT_CANCEL,
                                      row, col);
  }

  // Insert one codepoint into the live grid editor. Called from
  // NEUINativePaintedView's keyDown: while m.edit.active. Skips controls.
  void grid_painted_char_macos(WidgetData& wd, uint32_t cp)
  {
    auto& m = ensure_grid_model_macos(wd);
    if (!m.edit.active) return;
    if (cp < 0x20 || cp == 0x7F) return;
    char buf[4];
    int  n = neui_detail::te_encode_utf8(cp, buf);
    auto& te = m.edit.te;
    neui_detail::te_insert_utf8(te.text, te.cursor, te.sel_anchor,
                                  te.overwrite, buf, n, &m.edit.history);
    grid_repaint_macos(wd);
  }

  // Funnelled input kind. AppKit hands us one of these per native event.
  enum class GridMsg { Down, DblClick, Drag, Up, Move, Wheel, Key };

  static void grid_painted_msg_macos(WidgetData& wd, GridMsg kind,
                                       float lxf, float lyf,
                                       uint32_t keycode, uint32_t mods,
                                       int wheel_ticks)
  {
    using namespace neui_detail;
    if (!wd.session) return;
    auto& m   = ensure_grid_model_macos(wd);
    auto  cfg = grid_read_config(wd.attrs.get());

    int widget_w = 0, widget_h = 0;
    grid_widget_size_macos(wd, &widget_w, &widget_h);
    GridViewport vp = grid_compute_viewport(m, widget_w, widget_h,
                                              cfg.row_h, cfg.header_h);
    int lx = (int)lxf;
    int ly = (int)lyf;

    // Wheel - vertical scroll by N rows. macOS scrollingDeltaY is already
    // normalised to integer ticks by the caller; positive = swipe-down
    // (natural scrolling reveals earlier rows), matching win32's `-=`.
    if (kind == GridMsg::Wheel) {
      m.scroll_offset_y -= wheel_ticks;
      grid_clamp_scroll(m, vp, cfg.row_h);
      grid_repaint_macos(wd);
      return;
    }

    if (kind == GridMsg::Key) {
      // --- Edit-mode keys take priority over the nav switch ---
      if (m.edit.active) {
        auto& te    = m.edit.te;
        auto& hist  = m.edit.history;
        const bool shift = (mods & NEUI_KMOD_SHIFT) != 0;
        const bool ctrl  = (mods & NEUI_KMOD_CTRL)  != 0;
        switch (keycode) {
        case NEUI_KEY_RETURN: grid_commit_edit_macos(wd); return;
        case NEUI_KEY_ESCAPE: grid_cancel_edit_macos(wd); return;
        case NEUI_KEY_LEFT:
          te_move_left (te.text, te.cursor, te.sel_anchor, ctrl, shift, &hist);
          grid_repaint_macos(wd); return;
        case NEUI_KEY_RIGHT:
          te_move_right(te.text, te.cursor, te.sel_anchor, ctrl, shift, &hist);
          grid_repaint_macos(wd); return;
        case NEUI_KEY_HOME:
          te_move_home (te.text, te.cursor, te.sel_anchor, shift, &hist);
          grid_repaint_macos(wd); return;
        case NEUI_KEY_END:
          te_move_end  (te.text, te.cursor, te.sel_anchor, shift, &hist);
          grid_repaint_macos(wd); return;
        case NEUI_KEY_BACK:
          te_backspace     (te.text, te.cursor, te.sel_anchor, ctrl, &hist);
          grid_repaint_macos(wd); return;
        case NEUI_KEY_DELETE:
          te_delete_forward(te.text, te.cursor, te.sel_anchor, ctrl, &hist);
          grid_repaint_macos(wd); return;
        case NEUI_KEY_A:
          if (ctrl) {
            te_select_all(te.text, te.cursor, te.sel_anchor, &hist);
            grid_repaint_macos(wd);
          }
          return;
        case NEUI_KEY_C:
          if (ctrl) {
            std::string sel = te_selected_text(te.text, te.cursor, te.sel_anchor);
            if (!sel.empty())
              clipboard_set_text_macos(sel.c_str(), (uint32_t)sel.size());
          }
          return;
        case NEUI_KEY_X:
          if (ctrl) {
            std::string sel = te_selected_text(te.text, te.cursor, te.sel_anchor);
            if (!sel.empty()) {
              clipboard_set_text_macos(sel.c_str(), (uint32_t)sel.size());
              hist.mark(EditState{ te.text, te.cursor, te.sel_anchor },
                        EditHistory::None, true);
              te_erase_selection(te.text, te.cursor, te.sel_anchor);
              grid_repaint_macos(wd);
            }
          }
          return;
        case NEUI_KEY_V:
          if (ctrl) {
            int n = clipboard_get_text_macos(nullptr, 0);
            if (n > 0) {
              std::vector<char> buf((size_t)n);
              clipboard_get_text_macos(buf.data(), n);
              std::string paste(buf.data(), (size_t)(n > 0 ? n - 1 : 0));
              te_paste(te.text, te.cursor, te.sel_anchor, paste,
                       /*strip_newlines=*/true, &hist);
              grid_repaint_macos(wd);
            }
          }
          return;
        case NEUI_KEY_Z:
          if (ctrl) {
            if (shift) te_redo(te.text, te.cursor, te.sel_anchor, hist);
            else       te_undo(te.text, te.cursor, te.sel_anchor, hist);
            grid_repaint_macos(wd);
          }
          return;
        case NEUI_KEY_Y:
          if (ctrl) {
            te_redo(te.text, te.cursor, te.sel_anchor, hist);
            grid_repaint_macos(wd);
          }
          return;
        default:
          return;  // swallow other keys while editing
        }
      }

      int n_rows = (int)m.rows.size();
      int n_cols = (int)m.columns.size();
      if (n_rows == 0) return;

      // Walk visual order so Up / Down match the sorted display.
      grid_ensure_sort_clean(m);

      int prev_row = m.selected_row;
      int prev_col = m.selected_col;
      int vis = grid_visible_rows(vp, cfg.row_h);
      if (vis < 1) vis = 1;
      bool handled = true;
      switch (keycode) {
      case NEUI_KEY_UP: {
        int v = grid_selected_visual(m);
        grid_set_selected_visual(m, (v < 0) ? 0 : (v - 1));
        break;
      }
      case NEUI_KEY_DOWN: {
        int v = grid_selected_visual(m);
        grid_set_selected_visual(m, (v < 0) ? 0 : (v + 1));
        break;
      }
      case NEUI_KEY_PAGEUP: {
        int v = grid_selected_visual(m);
        grid_set_selected_visual(m, (v < 0) ? 0 : (v - vis));
        break;
      }
      case NEUI_KEY_PAGEDOWN: {
        int v = grid_selected_visual(m);
        grid_set_selected_visual(m, (v < 0) ? vis : (v + vis));
        break;
      }
      case NEUI_KEY_HOME:
        if (cfg.cell_focus && !(mods & NEUI_KMOD_CTRL)) {
          m.selected_col = (n_cols > 0) ? 0 : -1;
          if (m.selected_row < 0) grid_set_selected_visual(m, 0);
        } else {
          grid_set_selected_visual(m, 0);
          if (cfg.cell_focus) m.selected_col = (n_cols > 0) ? 0 : -1;
        }
        break;
      case NEUI_KEY_END:
        if (cfg.cell_focus && !(mods & NEUI_KMOD_CTRL)) {
          m.selected_col = (n_cols > 0) ? n_cols - 1 : -1;
          if (m.selected_row < 0) grid_set_selected_visual(m, n_rows - 1);
        } else {
          grid_set_selected_visual(m, n_rows - 1);
          if (cfg.cell_focus) m.selected_col = (n_cols > 0) ? n_cols - 1 : -1;
        }
        break;
      case NEUI_KEY_LEFT:
        if (cfg.cell_focus) {
          if (m.selected_col > 0) m.selected_col--;
          else if (m.selected_col < 0 && n_cols > 0) m.selected_col = 0;
          if (m.selected_row < 0) m.selected_row = 0;
        } else {
          m.scroll_offset_x -= grid_horizontal_step_px(m);
          grid_clamp_scroll(m, vp, cfg.row_h);
          grid_repaint_macos(wd);
          return;
        }
        break;
      case NEUI_KEY_RIGHT:
        if (cfg.cell_focus) {
          if (m.selected_col < n_cols - 1) {
            if (m.selected_col < 0) m.selected_col = 0;
            else                     m.selected_col++;
          }
          if (m.selected_row < 0) m.selected_row = 0;
        } else {
          m.scroll_offset_x += grid_horizontal_step_px(m);
          grid_clamp_scroll(m, vp, cfg.row_h);
          grid_repaint_macos(wd);
          return;
        }
        break;
      case NEUI_KEY_RETURN: {
        int r = m.selected_row;
        if (r < 0) return;
        // Cell-edit takes priority over ROW_ACTIVATED when the column is
        // editable and we're in cell-focus mode.
        if (cfg.cell_focus && m.selected_col >= 0 &&
            grid_try_begin_edit_macos(wd, r, m.selected_col)) {
          return;
        }
        grid_fire_row_activated_macos(wd, r);
        return;
      }
      default:
        handled = false; break;
      }
      if (!handled) return;
      // Keyboard row nav snaps back to exact row alignment (drops any
      // smooth-scroll fine offset left by a prior wheel gesture).
      m.scroll_px_offset = 0;
      if (cfg.cell_focus && m.selected_col >= 0)
        grid_ensure_cell_visible(m, vp, cfg.row_h, m.selected_row, m.selected_col);
      else
        grid_ensure_row_visible(m, vp, cfg.row_h, m.selected_row);
      if (m.selected_row != prev_row)
        grid_fire_row_selected_macos(wd, m.selected_row);
      if (cfg.cell_focus &&
          (m.selected_row != prev_row || m.selected_col != prev_col))
        grid_fire_cell_selected_macos(wd, m.selected_row, m.selected_col);
      grid_repaint_macos(wd);
      return;
    }

    // --- column-resize drag in progress ---
    if (m.column_resize_col >= 0) {
      if (kind == GridMsg::Drag) {
        int dx_log = lx - m.column_resize_start_x;  // both logical points
        int new_w  = m.column_resize_start_w + dx_log;
        int min_w  = grid_column_min_width(m, m.column_resize_col,
                                             cfg.col_min_w_def);
        if (new_w < min_w) new_w = min_w;
        if (new_w > 5000)  new_w = 5000;
        m.columns[(size_t)m.column_resize_col].width = new_w;
        grid_clamp_scroll(m, vp, cfg.row_h);
        [[NSCursor resizeLeftRightCursor] set];
        grid_repaint_macos(wd);
        return;
      }
      if (kind == GridMsg::Up) {
        int new_w = m.columns[(size_t)m.column_resize_col].width;
        int col   = m.column_resize_col;
        int old_w = m.column_resize_old_w;
        m.column_resize_col = -1;
        [[NSCursor arrowCursor] set];
        if (new_w != old_w) grid_fire_column_resized_macos(wd, col, old_w, new_w);
        grid_repaint_macos(wd);
        return;
      }
      return;
    }

    // --- vertical scrollbar drag in progress ---
    if (m.vert_drag.active) {
      if (kind == GridMsg::Up) { m.vert_drag.active = false; return; }
      if (kind == GridMsg::Drag) {
        int vis = grid_visible_rows(vp, cfg.row_h);
        ScrollbarGeom g = compute_scrollbar(vp.body_h, 0,
                                             (int)m.rows.size(), vis,
                                             m.vert_drag.start_position);
        int rel = ly - vp.body_y;
        m.scroll_offset_y = scrollbar_drag_apply(m.vert_drag, rel, g,
                                                  (int)m.rows.size(), vis);
        m.scroll_px_offset = 0;   // scrollbar drag = exact row alignment
        grid_clamp_scroll(m, vp, cfg.row_h);
        grid_repaint_macos(wd);
        return;
      }
      return;
    }

    // --- horizontal scrollbar drag in progress ---
    if (m.horz_drag.active) {
      if (kind == GridMsg::Up) { m.horz_drag.active = false; return; }
      if (kind == GridMsg::Drag) {
        int content_w = grid_total_content_width(m);
        ScrollbarGeom g = compute_scrollbar(vp.body_w, 0,
                                             content_w, vp.body_w,
                                             m.horz_drag.start_position);
        int rel = lx - vp.body_x;
        m.scroll_offset_x = scrollbar_drag_apply(m.horz_drag, rel, g,
                                                  content_w, vp.body_w);
        grid_clamp_scroll(m, vp, cfg.row_h);
        grid_repaint_macos(wd);
        return;
      }
      return;
    }

    // --- mouse move: cursor feedback for header divider ---
    if (kind == GridMsg::Move) {
      GridHit hit = grid_hit_test(m, vp, cfg.row_h,
                                   widget_w, widget_h, lx, ly);
      if (hit.region == GridHitRegion::HeaderDivider)
        [[NSCursor resizeLeftRightCursor] set];
      else
        [[NSCursor arrowCursor] set];
      return;
    }

    // --- button down / dbl-click ---
    if (kind == GridMsg::Down || kind == GridMsg::DblClick) {
      // Hit-test reads display_order; rebuild it first if dirty.
      grid_ensure_sort_clean(m);
      GridHit hit = grid_hit_test(m, vp, cfg.row_h, widget_w, widget_h, lx, ly);

      // --- Edit-mode click handling ---
      // Click inside the editing cell: swallow (editor stays open). Click
      // anywhere else: commit. If commit was rejected by validate_cell the
      // editor stays open and we swallow the click so the underlying grid
      // doesn't also act on it.
      if (m.edit.active) {
        bool on_editing_cell = (hit.region == GridHitRegion::Cell &&
                                hit.row == m.edit.row &&
                                hit.col == m.edit.col);
        if (on_editing_cell) return;
        if (!grid_commit_edit_macos(wd)) return;
        // Commit succeeded - fall through to normal click handling.
      }

      switch (hit.region) {
      case GridHitRegion::HeaderDivider:
        if (kind == GridMsg::Down) {
          m.column_resize_col     = hit.col;
          m.column_resize_start_x = lx;       // logical points
          m.column_resize_start_w = m.columns[(size_t)hit.col].width;
          m.column_resize_old_w   = m.column_resize_start_w;
          [[NSCursor resizeLeftRightCursor] set];
        }
        return;
      case GridHitRegion::Header:
        // Sort cycle on a sortable column header. Shift+click = add /
        // cycle a secondary level; plain click replaces the stack.
        if (kind == GridMsg::Down && grid_header_click_allowed(m, hit.col)) {
          bool shift = (mods & NEUI_KMOD_SHIFT) != 0;
          neui_grid_sort_dir_t new_dir =
            grid_apply_header_click(m, hit.col, shift);
          grid_repaint_macos(wd);
          grid_fire_sort_changed_macos(wd, hit.col, new_dir);
        }
        return;
      case GridHitRegion::VertScrollTrack: {
        int vis = grid_visible_rows(vp, cfg.row_h);
        ScrollbarGeom g = compute_scrollbar(vp.body_h, 0,
                                             (int)m.rows.size(), vis,
                                             m.scroll_offset_y);
        int rel = ly - vp.body_y;
        if (g.visible && rel >= g.thumb_pos && rel < g.thumb_pos + g.thumb_len) {
          m.vert_drag.active           = true;
          m.vert_drag.start_axis_coord = rel;
          m.vert_drag.start_position   = m.scroll_offset_y;
        } else if (g.visible) {
          int step = vis > 0 ? vis : 1;
          if (rel < g.thumb_pos) m.scroll_offset_y -= step;
          else                   m.scroll_offset_y += step;
          m.scroll_px_offset = 0;   // page step = exact row alignment
          grid_clamp_scroll(m, vp, cfg.row_h);
          grid_repaint_macos(wd);
        }
        return;
      }
      case GridHitRegion::HorzScrollTrack: {
        int content_w = grid_total_content_width(m);
        ScrollbarGeom g = compute_scrollbar(vp.body_w, 0,
                                             content_w, vp.body_w,
                                             m.scroll_offset_x);
        int rel = lx - vp.body_x;
        if (g.visible && rel >= g.thumb_pos && rel < g.thumb_pos + g.thumb_len) {
          m.horz_drag.active           = true;
          m.horz_drag.start_axis_coord = rel;
          m.horz_drag.start_position   = m.scroll_offset_x;
        } else if (g.visible) {
          int step = vp.body_w > 0 ? vp.body_w : 60;
          if (rel < g.thumb_pos) m.scroll_offset_x -= step;
          else                   m.scroll_offset_x += step;
          grid_clamp_scroll(m, vp, cfg.row_h);
          grid_repaint_macos(wd);
        }
        return;
      }
      case GridHitRegion::Cell: {
        if (kind == GridMsg::DblClick) {
          // Try opening the in-place editor first (mirrors ENTER); falls
          // back to ROW_ACTIVATED for non-editable cells / row-focus mode.
          if (!grid_try_begin_edit_macos(wd, hit.row, hit.col))
            grid_fire_row_activated_macos(wd, hit.row);
        } else {
          const GridCellOverride* ov = grid_find_override(m, hit.row, hit.col);
          bool cell_dis = ov && ov->has_enabled && !ov->enabled;
          int prev_row = m.selected_row;
          m.selected_row = hit.row;
          if (cfg.cell_focus) m.selected_col = hit.col;
          if (cell_dis) {
            if (m.selected_row != prev_row)
              grid_fire_row_selected_macos(wd, hit.row);
          } else {
            grid_click_ladder_macos(wd, hit.row, hit.col);
          }
        }
        grid_repaint_macos(wd);
        return;
      }
      case GridHitRegion::BodyEmpty:
        if (m.selected_row != -1) {
          m.selected_row = -1;
          m.selected_col = -1;
          grid_fire_row_selected_macos(wd, -1);
          grid_repaint_macos(wd);
        }
        return;
      default:
        return;
      }
    }

    if (kind == GridMsg::Up) {
      // Stray UP (drag started outside) - reset transient state.
      m.vert_drag.active = false;
      m.horz_drag.active = false;
      return;
    }
  }

} // namespace macos_host

// ---------------------------------------------------------------------------
// Module-private state.

namespace {

// Live APPWINDOW count. Hits 0 -> [NSApp stop:nil] + post wake-up.
int g_appwindow_count = 0;

bool g_nsapp_initialised = false;

void wake_app_event_pump()
{
  NSEvent* dummy = [NSEvent otherEventWithType:NSEventTypeApplicationDefined
                                       location:NSZeroPoint
                                  modifierFlags:0
                                      timestamp:0
                                   windowNumber:0
                                        context:nil
                                        subtype:0
                                          data1:0
                                          data2:0];
  [NSApp postEvent:dummy atStart:NO];
}

// Listener that re-resolves SF Symbol images on every checkbox after the
// user toggles system Appearance or Accent Color. NSImageSymbolConfiguration
// captures the resolved color at config time, so the cached NSImage doesn't
// auto-update; we have to rebuild it.
void refresh_all_checkbox_images(void* /*token*/)
{
  using namespace macos_host;
  for (auto& sptr : sessions) {
    if (!sptr) continue;
    auto& tree = sptr->_widgets;
    auto order = tree.release_order();
    for (uint32_t idx : order) {
      if (!tree.exists(idx)) continue;
      auto& wd = tree[idx];
      if (!wd.type || !wd.native_control) continue;
      bool is_checkbox = !std::strcmp(wd.type, NEUI_W_CHECKBOX)
                      || !std::strcmp(wd.type, NEUI_W_CHECKBOX3);
      if (!is_checkbox) continue;
      NSView* v = (__bridge NSView*)wd.native_control;
      if (![v isKindOfClass:[NSButton class]]) continue;
      int state = wd.attrs ? wd.attrs->get_int("neui.macoshost.checkstate",
                                                 NEUI_CHECK_UNCHECKED)
                           : NEUI_CHECK_UNCHECKED;
      ((NSButton*)v).image = checkbox_image_for_state(state);
    }
  }
}

void ensure_nsapp_initialised()
{
  if (g_nsapp_initialised) return;
  g_nsapp_initialised = true;
  [NSApplication sharedApplication];
  [NSApp setActivationPolicy:NSApplicationActivationPolicyRegular];
  neui_detail::ensure_theme_provider_macos();
  neui_detail::register_theme_listener(&refresh_all_checkbox_images, nullptr);
}

// Geometry + style-mask bodies shared with the xpl macOS glue
// (hosts/shared/macos/window_helpers_macos.h); thin local names kept so
// call sites stay terse.
NSRect logical_window_rect(int x, int y, int w, int h)
{ return neui_detail::logical_window_rect_macos(x, y, w, h); }

NSWindow* native_window_from(void* nh) { return (__bridge NSWindow*)nh; }
NSView*   native_view_from  (void* nh) { return (__bridge NSView*)nh; }

NSWindowStyleMask styles_for_appwindow()
{ return neui_detail::styles_for_appwindow_macos(); }

} // namespace

// ---------------------------------------------------------------------------
// NEUINativeContentView - flipped container so child widgets' frames use
// top-left logical coordinates (matches wd.x / wd.y / wd.width / wd.height).

@interface NEUINativeContentView : NSView<NSDraggingDestination>
@end

@implementation NEUINativeContentView
- (BOOL)isFlipped { return YES; }

// Make native text editing self-sufficient. Unlike the win32 native Edit
// control (which handles Ctrl+C/X/V/A itself), a Cocoa text field only
// receives CmdC/CmdX/CmdV/CmdA/CmdZ when a main-menu item claims those key
// equivalents - AppKit does not synthesize a standard Edit menu. Rather than
// force every client to add Cut/Copy/Paste items, route the standard editing
// shortcuts here through the same command path as menu / NEUI_API_COMMANDS.
//
// Order: NSApp matches main-menu key equivalents first, so a client-defined
// Edit > Undo (CmdZ) still wins; this override only fires for shortcuts the
// menu didn't claim. invoke_focused_command_macos returns false when no text
// responder consumes the action, so we fall through to super in that case.
- (BOOL)performKeyEquivalent:(NSEvent*)event
{
  if (event.modifierFlags & NSEventModifierFlagCommand) {
    NSString* chars = event.charactersIgnoringModifiers.lowercaseString;
    bool shift = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    uint32_t cmd = 0;
    if      ([chars isEqualToString:@"c"]) cmd = NEUI_CMD_COPY;
    else if ([chars isEqualToString:@"v"]) cmd = NEUI_CMD_PASTE;
    else if ([chars isEqualToString:@"x"]) cmd = NEUI_CMD_CUT;
    else if ([chars isEqualToString:@"a"]) cmd = NEUI_CMD_SELECT_ALL;
    else if ([chars isEqualToString:@"z"]) cmd = shift ? NEUI_CMD_REDO : NEUI_CMD_UNDO;
    if (cmd && macos_host::invoke_focused_command_macos(cmd))
      return YES;
  }
  return [super performKeyEquivalent:event];
}

// ---------------------------------------------------------------------------
// NSDraggingDestination - drag&drop drop-target routing for the native
// macOS host. The window's delegate carries the (Session, widget_index)
// the framework needs to dispatch DnD events.

// NSDragOperation <-> NEUI_DND_ACTION_* translation + the modifier-aware
// suggestion live in hosts/shared/macos/dnd_helpers_macos.h
// (dnd_suggested_from_nsop / dnd_nsop_from_action), shared with the xpl
// host's NEUIView.

- (NSDragOperation)draggingEntered:(id<NSDraggingInfo>)sender
{
  NEUINativeWindowDelegate* d = (NEUINativeWindowDelegate*)self.window.delegate;
  if (!d || !d->session) return NSDragOperationNone;
  NSPasteboard* pb = [sender draggingPasteboard];
  auto ml = neui_detail::pb_collect_mime_list_macos(pb);
  NSPoint p = [self convertPoint:[sender draggingLocation] fromView:nil];
  uint32_t suggested = neui_detail::dnd_suggested_from_nsop(
    [sender draggingSourceOperationMask]);
  uint32_t accepted = d->session->dispatch_dnd_enter(d->widget_index,
                                                       (int)p.x, (int)p.y,
                                                       ml.ptrs.data(),
                                                       (uint32_t)ml.ptrs.size(),
                                                       suggested, 0);
  return neui_detail::dnd_nsop_from_action(accepted);
}

- (NSDragOperation)draggingUpdated:(id<NSDraggingInfo>)sender
{
  NEUINativeWindowDelegate* d = (NEUINativeWindowDelegate*)self.window.delegate;
  if (!d || !d->session) return NSDragOperationNone;
  NSPasteboard* pb = [sender draggingPasteboard];
  auto ml = neui_detail::pb_collect_mime_list_macos(pb);
  NSPoint p = [self convertPoint:[sender draggingLocation] fromView:nil];
  uint32_t suggested = neui_detail::dnd_suggested_from_nsop(
    [sender draggingSourceOperationMask]);
  uint32_t accepted = d->session->dispatch_dnd_move(d->widget_index,
                                                      (int)p.x, (int)p.y,
                                                      ml.ptrs.data(),
                                                      (uint32_t)ml.ptrs.size(),
                                                      suggested, 0);
  return neui_detail::dnd_nsop_from_action(accepted);
}

- (void)draggingExited:(id<NSDraggingInfo>)sender
{
  (void)sender;
  NEUINativeWindowDelegate* d = (NEUINativeWindowDelegate*)self.window.delegate;
  if (d && d->session) d->session->dispatch_dnd_leave();
}

- (BOOL)performDragOperation:(id<NSDraggingInfo>)sender
{
  NEUINativeWindowDelegate* d = (NEUINativeWindowDelegate*)self.window.delegate;
  if (!d || !d->session) return NO;
  NSPasteboard* pb = [sender draggingPasteboard];
  neui_detail::DataItem item;
  neui_detail::pb_read_item_macos(pb, item);
  auto ml = neui_detail::pb_collect_mime_list_macos(pb);
  NSPoint p = [self convertPoint:[sender draggingLocation] fromView:nil];
  uint32_t suggested = neui_detail::dnd_suggested_from_nsop(
    [sender draggingSourceOperationMask]);
  uint32_t accepted = d->session->dispatch_dnd_drop(d->widget_index,
                                                      (int)p.x, (int)p.y,
                                                      ml.ptrs.data(),
                                                      (uint32_t)ml.ptrs.size(),
                                                      suggested, 0, &item);
  return accepted ? YES : NO;
}

@end

// ---------------------------------------------------------------------------
// NEUINativePaintedView - host for self-painted widgets (IMAGE, KNOB, future
// painted controls). Each instance owns its own CGContextState handle from
// neui_cg_backend::create_context. drawRect: calls set_current_frame +
// begin_frame, dispatches per-type paint, then end_frame.

@interface NEUINativePaintedView : NSView
{
@public
  uint32_t          widget_id;
  neui_render_ctx_t render_ctx;
  // KNOB drag state. Mirror of the xpl host's KnobWidget drag fields:
  // dragging gates the move handler; drag_prev_angle is the last
  // pointer-relative-to-center angle; drag_continuous is an unsnapped
  // accumulator so step-snapped values still respond to small deltas.
  bool              dragging;
  float             drag_prev_angle;
  float             drag_continuous;
  // Scroll-wheel accumulator (logical points for precise deltas, lines
  // otherwise) so trackpad / Magic Mouse swipes emit one tick per unit
  // rather than one tick per AppKit event. Used by KNOB / CUSTOMDRAW.
  CGFloat           wheel_accum_y;

  // GRID smooth-scroll: the kinetics state (raw px integral, last-commit, and
  // momentum-suppression flag) lives in the widget's GridModel.scroll_kin so
  // it is shared with the elastic math in grid_model.h. The view only owns the
  // spring-back animation timer.
  NSTimer*          grid_bounce_timer;

  // SECTION smooth-scroll: per-axis kinetics integrators live in the
  // widget's SectionScrollState.kin_v/_h (hosts/shared/widget_section_scroll.h).
  // The view only owns the 60 Hz spring-back animation timer.
  NSTimer*          section_bounce_timer;
}
@end

// Inner body view for a scrolling SECTION. Positioned at the section's body
// rect (below the title-chip band, inside the scrollbar gutter); the
// section's tree children parent to this view so AppKit's default subview
// clipping confines them to the body - they can no longer overpaint the chip
// band or the scrollbar gutter, and the body-local child coords match the
// win32 host's body_hwnd contract. isFlipped = YES matches the section view's
// Y-down convention.
//
// Unlike the win32 body HWND, this view is a *transparent clipping container*
// and does NOT paint its own background: the section's own painted view
// (drawRect: SECTION branch -> paint_section) already fills the body rect with
// the resolved bg, and that shows through here. An opaque NSRectFill in this
// view's drawRect: disturbed the non-layer-backed sibling rendering of the
// section + toolbar controls (they stopped drawing entirely), so the body
// view stays purely structural - AppKit clips children to its bounds whether
// or not it draws. Equivalent end result to the win32 body HWND minus the
// self-fill.
@interface NEUISectionBodyView : NSView
{
@public
  uint32_t widget_id;  // the owning SECTION's widget id
}
@end

@implementation NEUISectionBodyView
- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque  { return NO; }
@end

// KNOB drag tunables - mirror of the xpl host's KnobWidget constants
// (hosts/crossplatform/host.cpp around the KnobWidget block).
static constexpr float NEUI_KNOB_SWEEP_RAD   = 4.71238898f;  // 1.5*PI (270deg)
static constexpr float NEUI_KNOB_DEAD_ZONE_R = 4.0f;          // logical px
static constexpr float NEUI_KNOB_FINE_SCALE  = 0.2f;          // Shift = 1/5

// Points of accumulated precise scroll delta = one tick. NSEvent reports
// precise (trackpad / Magic Mouse) deltas in points, streaming dozens of
// events per swipe. Picking a value close to a typical line height keeps a
// gentle swipe in single-digit ticks (matches a few mouse-wheel notches).
static constexpr CGFloat NEUI_WHEEL_PRECISE_POINTS_PER_TICK = 16.0;

static float neui_knob_wrap_pi(float d)
{
  const float PI    = 3.14159265358979323846f;
  const float TWOPI = 2.0f * PI;
  while (d >  PI) d -= TWOPI;
  while (d < -PI) d += TWOPI;
  return d;
}

static float neui_clamp01(float v)
{
  if (v < 0.0f) return 0.0f;
  if (v > 1.0f) return 1.0f;
  return v;
}

// Snap to evenly-spaced positions on [0..1] when steps >= 2. Matches the
// xpl host's snap_to_steps so the native + xpl knobs feel identical.
static float neui_snap_to_steps(float v, int steps)
{
  if (steps < 2) return v;
  float n = (float)(steps - 1);
  return std::round(v * n) / n;
}

// GRID elastic-scroll math + tuning live in hosts/shared/grid_model.h
// (grid_scroll_* + GRID_SCROLL_* constants) so the native + xpl macOS hosts
// behave identically. This view only supplies the NSEvent plumbing + the
// spring-back animation timer.

@implementation NEUINativePaintedView

- (BOOL)isFlipped { return YES; }
- (BOOL)isOpaque  { return NO; }
- (BOOL)acceptsFirstMouse:(NSEvent*)event { (void)event; return YES; }

// CUSTOMDRAW (forwards keys to the client) and GRID (handles arrow / page /
// home / end / return nav itself) take keyboard focus. KNOB / IMAGE / SECTION
// don't need key input.
- (BOOL)acceptsFirstResponder
{
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd || !wd->type) return NO;
  return !strcmp(wd->type, NEUI_W_CUSTOMDRAW) ||
         !strcmp(wd->type, NEUI_W_GRID);
}

// GRID commits an open in-place cell editor on focus loss so Tab /
// click-elsewhere don't leave a stale editor over a widget that no
// longer has the keyboard. On validate-reject we fall back to cancel
// rather than fighting AppKit's responder change.
- (BOOL)resignFirstResponder
{
  auto* wd = macos_host::widget_for_id(widget_id);
  if (wd && wd->type && !strcmp(wd->type, NEUI_W_GRID)) {
    auto& m = macos_host::ensure_grid_model_macos(*wd);
    if (m.edit.active) {
      if (!macos_host::grid_commit_edit_macos(*wd))
        macos_host::grid_cancel_edit_macos(*wd);
    }
  }
  return [super resignFirstResponder];
}

- (void)keyDown:(NSEvent*)event
{
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  // GRID owns its keyboard navigation (arrows / page / home / end / return).
  if (wd && sess && wd->enabled && wd->type && !strcmp(wd->type, NEUI_W_GRID)) {
    [self gridStopBounce];   // user input cancels any spring-back animation
    auto& gm = macos_host::ensure_grid_model_macos(*wd);
    gm.scroll_kin.suppress_momentum = false;
    uint32_t kc   = neui_detail::mac_keycode_to_neui(event.keyCode);
    uint32_t mods = neui_detail::mac_modifiers_to_neui(event.modifierFlags);
    macos_host::grid_painted_msg_macos(*wd, macos_host::GridMsg::Key,
                                        0, 0, kc, mods, 0);
    // Feed printable text into the in-place cell editor when active. Skip
    // when Command is held (those are shortcuts, not text input) and skip
    // private-use function-key codepoints (arrows, F-keys); those came in
    // via the GridMsg::Key path already.
    if (gm.edit.active &&
        !(event.modifierFlags & NSEventModifierFlagCommand)) {
      NSString* chars = event.characters;
      NSUInteger i = 0;
      while (i < chars.length) {
        unichar c = [chars characterAtIndex:i];
        uint32_t cp;
        if (c >= 0xD800 && c <= 0xDBFF && i + 1 < chars.length) {
          unichar lo = [chars characterAtIndex:i + 1];
          cp = 0x10000u + ((uint32_t)(c - 0xD800) << 10) + (uint32_t)(lo - 0xDC00);
          i += 2;
        } else {
          cp = c; i += 1;
        }
        if (cp >= 0xF700 && cp <= 0xF8FF) continue;  // function-key range
        macos_host::grid_painted_char_macos(*wd, cp);
      }
    }
    return;
  }
  bool cd = wd && sess && wd->emit_events && wd->type
            && !strcmp(wd->type, NEUI_W_CUSTOMDRAW);
  if (!cd) { [super keyDown:event]; return; }

  uint32_t mods = neui_detail::mac_modifiers_to_neui(event.modifierFlags);
  uint32_t kc   = neui_detail::mac_keycode_to_neui(event.keyCode);
  neui_event_t kd = {};
  kd.type                = NEUI_EVENT_KEYDOWN;
  kd.data.key.widget     = { wd->widget_id };
  kd.data.key.keycode    = kc;
  kd.data.key.modifiers  = mods;
  sess->dispatch_event(&kd);
  macos_host::dispatch_behavior_key(widget_id, kc, mods);

  // KEYCHAR for the produced text. Skip when Command is held (those are
  // shortcuts, not text - matches win32's WM_CHAR behaviour) and skip the
  // NSFunctionKey private-use range (arrows / F-keys come via KEYDOWN only).
  NSString* chars = event.characters;
  if (chars.length > 0 && !(event.modifierFlags & NSEventModifierFlagCommand)) {
    NSUInteger i = 0;
    while (i < chars.length) {
      unichar c = [chars characterAtIndex:i];
      uint32_t cp;
      if (c >= 0xD800 && c <= 0xDBFF && i + 1 < chars.length) {
        unichar lo = [chars characterAtIndex:i + 1];
        cp = 0x10000u + ((uint32_t)(c - 0xD800) << 10) + (uint32_t)(lo - 0xDC00);
        i += 2;
      } else {
        cp = c; i += 1;
      }
      if (cp >= 0xF700 && cp <= 0xF8FF) continue;  // function-key range
      neui_event_t kc = {};
      kc.type               = NEUI_EVENT_KEYCHAR;
      kc.data.key.widget    = { wd->widget_id };
      kc.data.key.keycode   = cp;
      kc.data.key.modifiers = mods;
      sess->dispatch_event(&kc);
    }
  }
}

- (void)keyUp:(NSEvent*)event
{
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  bool cd = wd && sess && wd->emit_events && wd->type
            && !strcmp(wd->type, NEUI_W_CUSTOMDRAW);
  if (!cd) { [super keyUp:event]; return; }
  neui_event_t ku = {};
  ku.type               = NEUI_EVENT_KEYUP;
  ku.data.key.widget    = { wd->widget_id };
  ku.data.key.keycode   = neui_detail::mac_keycode_to_neui(event.keyCode);
  ku.data.key.modifiers = neui_detail::mac_modifiers_to_neui(event.modifierFlags);
  sess->dispatch_event(&ku);
}

- (void)dealloc
{
  // Stop any in-flight GRID / SECTION spring-back animation (the timer
  // retains self, so it normally self-terminates first; this is belt-
  // and-suspenders).
  [self gridStopBounce];
  [self sectionStopBounce];
  // The IMAGE bitmap now lives in the session's MacOSAssetManager, not on
  // the view; its per-ctx GPU cache for this render_ctx is dropped by
  // release_native_control_macos before teardown. Here we only destroy the
  // view's own render context.
  auto* backend = neui_cg_backend::get_backend();
  if (backend && render_ctx)
    backend->destroy_context(render_ctx);
  render_ctx = nullptr;
}

- (void)drawRect:(NSRect)dirtyRect
{
  (void)dirtyRect;
  auto* backend = neui_cg_backend::get_backend();
  if (!backend || !render_ctx) return;
  CGContextRef cg = (CGContextRef)[[NSGraphicsContext currentContext] CGContext];
  if (!cg) return;

  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess) return;

  NSSize sz = self.bounds.size;
  neui_cg_backend::set_current_frame(render_ctx, (void*)cg,
                                      (float)sz.width, (float)sz.height);
  // Clear with the panel-bg colour matching the rest of the window. Same
  // approach as the xpl host's paint_frame. SECTION uses a transparent
  // clear so the un-painted header band shows the parent's pixels.
  bool is_section = macos_host::is_section_like(wd->type);
  bool is_tabview = wd->type && !strcmp(wd->type, NEUI_W_TABVIEW);
  // SECTION / TABPAGE / TABVIEW clear transparent so the shared paint helper
  // (which leaves the chip-band / strip-gutter area unpainted) shows the
  // parent's pixels through; other painted widgets clear to panel_bg / an
  // explicit NEUI_ATTR_BACKGROUND.
  uint32_t clear = (is_section || is_tabview)
    ? 0x00000000
    : neui_detail::color(neui_detail::ColorRole::panel_bg);
  if (!is_section && !is_tabview && wd->attrs && wd->attrs->has(NEUI_ATTR_BACKGROUND))
    clear = (uint32_t)wd->attrs->get_int(NEUI_ATTR_BACKGROUND, 0);
  backend->begin_frame(render_ctx, clear);

  // Disabled painted widgets (IMAGE / KNOB / CUSTOMDRAW / SECTION) draw their
  // content at half opacity over the (full-opacity) background clear, matching
  // the xpl host's per-widget push_alpha(0.5) dim. begin_frame resets the
  // alpha stack, so this must be pushed after it; popped before end_frame.
  bool dim_disabled = !wd->enabled;
  if (dim_disabled) backend->push_alpha(render_ctx, 0.5f);

  if (wd->type && !strcmp(wd->type, NEUI_W_IMAGE)) {
    // Resolve the widget's bitmap asset (set via a set_text path or a
    // set_asset handle) against the session asset manager. The lazy per-ctx
    // GPU upload + draw is delegated to macos_painter_draw_asset_thunk - the
    // same path CUSTOMDRAW + compound use - so there is no per-view cache.
    // We only compute the aspect-fit destination rect and rotation here.
    if (wd->image_asset.id != asset_none.id &&
        ((wd->image_asset.id >> 16) & 0xffff) == (sess->session_id() & 0xffff)) {
      auto* entry = sess->_asset_manager.get_slot(wd->image_asset.id & 0xffff);
      if (entry && entry->kind == NEUI_ASSET_KIND_BITMAP && entry->scale > 0.0f) {
        // Aspect-preserving fit (letterbox / pillarbox, centred). Same shape
        // as the other hosts.
        float vw = (float)sz.width, vh = (float)sz.height;
        float bw = (float)entry->width_px  / entry->scale;
        float bh = (float)entry->height_px / entry->scale;
        if (bw > 0.0f && bh > 0.0f && vw > 0.0f && vh > 0.0f) {
          float scale = (bw / bh > vw / vh) ? (vw / bw) : (vh / bh);
          float dw = bw * scale, dh = bh * scale;
          float dx = (vw - dw) * 0.5f, dy = (vh - dh) * 0.5f;
          // Honour NEUI_ATTR_ROTATION via the renderer transform stack -
          // same shape as the xpl host's IMAGE paint path.
          float rot = wd->attrs ? wd->attrs->get_float(NEUI_ATTR_ROTATION, 0.0f) : 0.0f;
          if (rot != 0.0f) {
            backend->push_transform(render_ctx);
            backend->translate(render_ctx, dx + dw * 0.5f, dy + dh * 0.5f);
            backend->rotate(render_ctx, rot);
            backend->translate(render_ctx, -dw * 0.5f, -dh * 0.5f);
            macos_host::macos_painter_draw_asset_thunk(
              sess, backend, render_ctx, wd->image_asset, 0, 0, dw, dh,
              0xFFFFFFFFu);
            backend->pop_transform(render_ctx);
          } else {
            macos_host::macos_painter_draw_asset_thunk(
              sess, backend, render_ctx, wd->image_asset, dx, dy, dw, dh,
              0xFFFFFFFFu);
          }
        }
      }
    }
  } else if (wd->type && !strcmp(wd->type, NEUI_W_KNOB)) {
    // Fire pre-update so the client can refresh NEUI_ATTR_VALUE_TEXT (etc.)
    // before paint_knob reads the value, matching the xpl host's pattern.
    if (wd->emit_events) {
      neui_event_t pe = {};
      pe.type = NEUI_EVENT_WIDGET_PREUPDATE;
      pe.data.preupdate.widget = { wd->widget_id };
      sess->dispatch_event(&pe);
    }
    float value = wd->attrs ? wd->attrs->get_float(NEUI_PARAM_VALUE, 0.0f) : 0.0f;
    int   steps = wd->attrs ? wd->attrs->get_int  (NEUI_ATTR_STEPS,  0   ) : 0;
    const char* polarity_str = wd->attrs ? wd->attrs->get_string(NEUI_ATTR_POLARITY) : nullptr;
    const char* value_text   = wd->attrs ? wd->attrs->get_string(NEUI_ATTR_VALUE_TEXT) : nullptr;
    auto polarity = neui_detail::parse_knob_polarity(polarity_str);

    neui_detail::paint_knob(backend, render_ctx,
                             0, 0, (float)sz.width, (float)sz.height,
                             value, /*focused*/false, polarity, steps, value_text,
                             wd->attrs.get());
  } else if (wd->type && !strcmp(wd->type, NEUI_W_CUSTOMDRAW)) {
    // CUSTOMDRAW dispatch - either paint the attached compound or
    // forward a WIDGET_PAINT event. Mirror of paint_customdraw_w32.
    // Outer push_transform + push_clip(0..w,0..h) so client state
    // changes can't bleed past the widget rect.
    if (backend->push_transform) backend->push_transform(render_ctx);
    if (backend->push_clip)      backend->push_clip(render_ctx, 0.0f, 0.0f,
                                                     (float)sz.width, (float)sz.height);

    neui_painter painter{};
    painter.backend          = backend;
    painter.ctx              = render_ctx;
    painter.host_token       = sess;
    painter.draw_asset_thunk = &macos_host::macos_painter_draw_asset_thunk;

    // Resolve compound (if any). asset_none -> no compound -> dispatch
    // WIDGET_PAINT.
    neui_detail::CompoundAsset* ca = nullptr;
    if (wd->compound_asset.id != asset_none.id &&
        ((wd->compound_asset.id >> 16) & 0xffff) == (sess->session_id() & 0xffff))
    {
      auto* e = sess->_asset_manager.get_slot(wd->compound_asset.id & 0xffff);
      if (e && e->kind == NEUI_ASSET_KIND_COMPOUND && e->compound) ca = e->compound.get();
    }

    if (ca) {
      const neui_detail::AttrBag* bag = neui_detail::attrs_readonly(wd->attrs);
      uint32_t state_mask = neui_detail::compose_widget_state(
                              wd->enabled, wd->hovered, wd->pressed);
      neui_detail::paint_compound_below(&painter, *ca,
                                          (float)sz.width, (float)sz.height, bag,
                                          state_mask);
      neui_detail::paint_compound_above(&painter, *ca,
                                          (float)sz.width, (float)sz.height, bag,
                                          state_mask);
    } else if (wd->emit_events) {
      bool focused = (self.window.isKeyWindow
                       && self.window.firstResponder == self);
      neui_event_t ev{};
      ev.type = NEUI_EVENT_WIDGET_PAINT;
      ev.data.paint.widget.id   = wd->widget_id;
      ev.data.paint.painter_api = &neui_detail::k_painter_api;
      ev.data.paint.p           = &painter;
      ev.data.paint.width       = (float)sz.width;
      ev.data.paint.height      = (float)sz.height;
      ev.data.paint.focused     = focused;
      sess->dispatch_event(&ev);
    }

    if (backend->pop_clip)      backend->pop_clip(render_ctx);
    if (backend->pop_transform) backend->pop_transform(render_ctx);
  } else if (wd->type && !strcmp(wd->type, NEUI_W_GRID)) {
    // GRID: the shared paint pass owns the whole surface (it issues an
    // unconditional body fill_rect, so the begin_frame clear above is just
    // overpainted). Mirror of paint_grid_w32.
    auto& m = macos_host::ensure_grid_model_macos(*wd);
    bool focused = (self.window.isKeyWindow
                     && self.window.firstResponder == self);
    neui_detail::paint_grid(backend, render_ctx,
                             0.0f, 0.0f, (float)sz.width, (float)sz.height,
                             m, wd->attrs.get(), focused);
  } else if (is_section) {
    // SECTION body + title chip. The shared helper leaves the band's
    // non-chip area UNPAINTED (transparent clear above) so the parent's
    // pixels show through - matches the xpl host's visual.
    //
    // Direction-aware lift: SECTION_BG_LIFT (+24) lifts the frame_bg
    // towards white, but on macOS in light mode frame_bg already lives
    // near white (windowBackgroundColor ~= 0xECECEC, often 0xFFFFFF under
    // newer appearances). When the lift saturates, the section becomes
    // invisible against the NSWindow background. Detect that and shade
    // down instead so the section reads as a depressed panel.
    uint32_t bg = macos_host::section_resolve_bg_argb(*wd);
    // A TABPAGE is a chip-less section: section_effective_*_macos return
    // ""/"none" so paint_section fills the whole rect with no header band.
    const char* align = macos_host::section_effective_align_macos(*wd);
    uint32_t text_argb = neui_detail::color(neui_detail::ColorRole::text_primary);
    neui_detail::paint_section(backend, render_ctx,
                                 0.0f, 0.0f, (float)sz.width, (float)sz.height,
                                 macos_host::section_effective_text_macos(*wd),
                                 bg, align, text_argb,
                                 wd->attrs.get());

    // Late-refresh in case set_string(SCROLL_MODE) ran without the explicit
    // refresh hook firing - cheap when state already matches. Mirror of the
    // xpl host's SectionWidget::paint safety net.
    macos_host::section_refresh_scroll_state_macos(*wd);
    macos_host::section_compute_layout_macos(*wd);

    if (wd->section_scroll_state &&
        (wd->section_last_layout.vert_sb_shown ||
         wd->section_last_layout.horz_sb_shown)) {
      uint32_t sep   = neui_detail::color(neui_detail::ColorRole::scrollbar_separator);
      uint32_t track = neui_detail::color(neui_detail::ColorRole::scrollbar_track);
      uint32_t thumb = neui_detail::color(neui_detail::ColorRole::scrollbar_thumb);
      neui_detail::paint_section_scrollbars(backend, render_ctx,
                                              wd->section_last_layout,
                                              *wd->section_scroll_state,
                                              sep, track, thumb);
    }
  } else if (is_tabview) {
    // TABVIEW: chip strip + body fill + tab-outline border, then size the
    // selected page to the body rect + hide the others. Mirror of the xpl
    // host's TabViewWidget::paint.
    using neui_detail::ColorRole;

    const char* pos = wd->attrs ? wd->attrs->get_string(NEUI_ATTR_TAB_POSITION) : nullptr;
    auto tp = neui_detail::parse_tab_position(pos);
    wd->tab_edge = tp.edge;

    std::vector<uint32_t> pages;
    macos_host::tabview_collect_pages_macos(*wd, pages);
    int count = (int)pages.size();
    if (wd->tab_selected >= count) wd->tab_selected = count > 0 ? count - 1 : 0;
    if (wd->tab_selected < 0)      wd->tab_selected = 0;

    // Measure each page's tab label (chip widths + auto vertical strip) +
    // collect per-chip colours / labels.
    neui_detail::EffectiveFont ef =
      neui_detail::read_widget_font(wd->attrs.get(), neui_detail::TAB_CHIP_FONT);
    std::vector<float>       widths(count, 0.0f);
    std::vector<std::string> label_store(count);
    std::vector<const char*> labels(count, "");
    std::vector<uint32_t>    chip_bg(count, 0), chip_text(count, 0);
    if (backend->measure_text) neui_detail::push_widget_font(backend, render_ctx, ef);
    for (int i = 0; i < count; ++i) {
      auto& pw = sess->_widgets[pages[i]];
      label_store[i] = pw.text;
      labels[i]      = label_store[i].c_str();
      if (backend->measure_text)
        widths[i] = backend->measure_text(render_ctx, labels[i], -1, ef.size);
      if (pw.attrs) {
        chip_bg[i]   = (uint32_t)pw.attrs->get_int(NEUI_ATTR_TAB_CHIP_BG_COLOR, 0);
        chip_text[i] = (uint32_t)pw.attrs->get_int(NEUI_ATTR_TAB_CHIP_TEXT_COLOR, 0);
      }
    }
    if (backend->measure_text) neui_detail::pop_widget_font(backend, render_ctx, ef);

    float explicit_strip = wd->attrs
                    ? (float)wd->attrs->get_int(NEUI_ATTR_TAB_STRIP_SIZE, 0) : 0.0f;
    float strip = neui_detail::tab_resolve_strip_size(tp.edge, explicit_strip,
                                                      widths.data(), count);
    neui_detail::TabViewLayout L =
      neui_detail::compute_tabview_layout((float)sz.width, (float)sz.height,
                                           tp.edge, strip);

    wd->tab_chips.assign(count, neui_detail::TabChip{});
    if (count > 0 && tp.edge != neui_detail::TabEdge::None)
      neui_detail::layout_tab_chips(L, tp.edge, tp.align, widths.data(),
                                     count, wd->tab_chips.data());

    // Body background: active page's NEUI_ATTR_BACKGROUND, else the tabview's,
    // else a panel shade. The strip area beside the chips stays transparent
    // unless NEUI_ATTR_TAB_STRIP_BG_COLOR is set (no whole-rect fill).
    uint32_t body_bg = neui_detail::shade(neui_detail::color(ColorRole::frame_bg),
                                           neui_detail::SECTION_BG_LIFT);
    if (wd->attrs && wd->attrs->has(NEUI_ATTR_BACKGROUND))
      body_bg = (uint32_t)wd->attrs->get_int(NEUI_ATTR_BACKGROUND, 0);
    if (count > 0) {
      auto& ap = sess->_widgets[pages[wd->tab_selected]];
      if (ap.attrs && ap.attrs->has(NEUI_ATTR_BACKGROUND))
        body_bg = (uint32_t)ap.attrs->get_int(NEUI_ATTR_BACKGROUND, 0);
    }
    uint32_t inactive     = neui_detail::shade(body_bg, -18);
    uint32_t default_text = neui_detail::color(ColorRole::text_primary);
    uint32_t content_border = (wd->attrs && wd->attrs->has(NEUI_ATTR_TAB_BORDER_COLOR))
                        ? (uint32_t)wd->attrs->get_int(NEUI_ATTR_TAB_BORDER_COLOR, 0) : 0;
    float border_w = wd->attrs
                       ? (float)wd->attrs->get_int(NEUI_ATTR_TAB_BORDER_WIDTH, 0) : 0.0f;
    if (border_w <= 0.0f) border_w = 1.0f;
    uint32_t sep_color = content_border ? content_border
                                        : neui_detail::color(ColorRole::border);
    uint32_t strip_bg = (wd->attrs && wd->attrs->has(NEUI_ATTR_TAB_STRIP_BG_COLOR))
                        ? (uint32_t)wd->attrs->get_int(NEUI_ATTR_TAB_STRIP_BG_COLOR, 0) : 0;
    float chip_radius = wd->attrs
                          ? (float)wd->attrs->get_int(NEUI_ATTR_TAB_CHIP_RADIUS, 0) : 0.0f;

    neui_detail::paint_tabview(backend, render_ctx,
                                0.0f, 0.0f, (float)sz.width, (float)sz.height,
                                L, tp.edge, wd->tab_chips.data(), count,
                                wd->tab_selected, -1,
                                labels.data(), chip_bg.data(), chip_text.data(),
                                body_bg, default_text, inactive,
                                sep_color, border_w, strip_bg, content_border,
                                chip_radius, wd->attrs.get());

    // Cache the content rect so page geometry (incl. auto vertical strip)
    // stays consistent outside paint, then size the selected page + show it.
    wd->section_last_layout = neui_detail::SectionLayout{};
    wd->section_last_layout.body_x = (int)L.body_x;
    wd->section_last_layout.body_y = (int)L.body_y;
    wd->section_last_layout.body_w = (int)L.body_w;
    wd->section_last_layout.body_h = (int)L.body_h;
    macos_host::tabview_apply_page_geometry_macos(*wd);
  }

  if (dim_disabled) backend->pop_alpha(render_ctx);

  backend->end_frame(render_ctx);
}

// ---------------------------------------------------------------------------
// KNOB mouse handling. Same shape as xpl_host::KnobWidget::on_mouse_event.

- (BOOL)isKnob
{
  auto* wd = macos_host::widget_for_id(widget_id);
  return wd && wd->type && !strcmp(wd->type, NEUI_W_KNOB);
}

// Read NEUI_PARAM_VALUE clamped to [0..1]. Step-snapping is already applied
// when the value was last written, so reading is just a fetch + clamp.
- (float)knobValue
{
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd || !wd->attrs) return 0.0f;
  return neui_clamp01(wd->attrs->get_float(NEUI_PARAM_VALUE, 0.0f));
}

- (int)knobSteps
{
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd || !wd->attrs) return 0;
  return wd->attrs->get_int(NEUI_ATTR_STEPS, 0);
}

// Write a new value: clamp + snap, store on attrs, fire VALUE_CHANGED if
// the snapped value actually moved. Mirrors xpl's widget_set_value_user.
- (void)setKnobValueFromUser:(float)v
{
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess) return;
  v = neui_snap_to_steps(neui_clamp01(v), [self knobSteps]);
  float old = [self knobValue];
  if (v == old) return;
  neui_detail::ensure_attrs(wd->attrs).set_float(NEUI_PARAM_VALUE, v);
  if (wd->emit_events) {
    neui_event_t ev = {};
    ev.type              = NEUI_EVENT_VALUE_CHANGED;
    ev.data.value.widget = { wd->widget_id };
    ev.data.value.value  = v;
    sess->dispatch_event(&ev);
  }
  [self setNeedsDisplay:YES];
}

- (NSPoint)knobCenter
{
  NSSize sz = self.bounds.size;
  return NSMakePoint(sz.width * 0.5, sz.height * 0.5);
}

- (NSPoint)localPointForKnobEvent:(NSEvent*)event
{
  // Pointer in our (flipped, top-left-origin) view coordinates.
  return [self convertPoint:event.locationInWindow fromView:nil];
}

// --- CUSTOMDRAW raw-input plumbing -----------------------------------------
// CUSTOMDRAW widgets forward pointer input to the client as NEUI_EVENT_MOUSE_*
// (parity with the win32 host's subclass proc). KNOB drives its own value and
// IMAGE / SECTION are non-interactive, so only CUSTOMDRAW opts in here.

- (bool)customDrawWantsInput
{
  auto* wd = macos_host::widget_for_id(widget_id);
  return wd && wd->enabled && wd->emit_events && wd->type
         && !strcmp(wd->type, NEUI_W_CUSTOMDRAW);
}

// Returns the WidgetData* if this view is an enabled GRID (so it should run
// the shared grid input dispatch), nullptr otherwise.
- (macos_host::WidgetData*)gridInputWidget
{
  auto* wd = macos_host::widget_for_id(widget_id);
  if (wd && wd->enabled && wd->type && !strcmp(wd->type, NEUI_W_GRID))
    return wd;
  return nullptr;
}

- (NSPoint)localPoint:(NSEvent*)event
{
  // Widget-local, flipped (top-left origin) - matches the WIDGET_PAINT origin.
  return [self convertPoint:event.locationInWindow fromView:nil];
}

- (void)dispatchMouse:(neui_event_type_t)type at:(NSPoint)p buttonmap:(uint32_t)bmap
{
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess) return;
  neui_event_t ev = {};
  ev.type                 = type;
  ev.data.mouse.widget    = { wd->widget_id };
  ev.data.mouse.x         = (int)p.x;
  ev.data.mouse.y         = (int)p.y;
  ev.data.mouse.buttonmap = bmap;
  sess->dispatch_event(&ev);
  // Run the behavior pass after the client has had first chance. The
  // dispatch is a no-op when no behavior asset is attached.
  macos_host::dispatch_behavior_mouse(widget_id, &ev, (float)p.x, (float)p.y);
}

// Tracking area drives MOUSE_MOVE / ENTER / LEAVE. Rebuilt whenever the view
// geometry changes (AppKit calls this after every setFrame:).
- (void)updateTrackingAreas
{
  [super updateTrackingAreas];
  for (NSTrackingArea* ta in [self.trackingAreas copy])
    [self removeTrackingArea:ta];
  NSTrackingArea* ta = [[NSTrackingArea alloc]
    initWithRect:self.bounds
    options:(NSTrackingMouseEnteredAndExited | NSTrackingMouseMoved
             | NSTrackingActiveInKeyWindow)
    owner:self userInfo:nil];
  [self addTrackingArea:ta];
}

- (void)mouseEntered:(NSEvent*)event
{
  if (![self customDrawWantsInput]) { [super mouseEntered:event]; return; }
  if (auto* wd = macos_host::widget_for_id(widget_id); wd && !wd->hovered) {
    wd->hovered = true;
    macos_host::macos_invalidate_if_state_filtered_compound(wd);
  }
  [self dispatchMouse:NEUI_EVENT_MOUSE_ENTER at:[self localPoint:event] buttonmap:0];
}
- (void)mouseExited:(NSEvent*)event
{
  if (![self customDrawWantsInput]) { [super mouseExited:event]; return; }
  if (auto* wd = macos_host::widget_for_id(widget_id); wd && wd->hovered) {
    wd->hovered = false;
    macos_host::macos_invalidate_if_state_filtered_compound(wd);
  }
  [self dispatchMouse:NEUI_EVENT_MOUSE_LEAVE at:[self localPoint:event] buttonmap:0];
}
- (void)mouseMoved:(NSEvent*)event
{
  if (auto* gwd = [self gridInputWidget]) {
    NSPoint p = [self localPoint:event];
    macos_host::grid_painted_msg_macos(*gwd, macos_host::GridMsg::Move,
                                        (float)p.x, (float)p.y, 0, 0, 0);
    return;
  }
  if (![self customDrawWantsInput]) { [super mouseMoved:event]; return; }
  [self dispatchMouse:NEUI_EVENT_MOUSE_MOVE at:[self localPoint:event] buttonmap:0];
}

// Resolve the SECTION + state pointer for the painted view if this widget
// is a scrolling section. Used by mouseDown / Dragged / Up to drive the
// shared scrollbar drag math.
- (macos_host::WidgetData*)sectionInputWidget
{
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd || !wd->type) return nullptr;
  if (!macos_host::is_section_like(wd->type)) return nullptr;
  if (!wd->section_scroll_state) return nullptr;
  return wd;
}

// Resolve the TABVIEW WidgetData for this painted view, or nullptr. Used by
// mouseDown to hit-test the chip strip.
- (macos_host::WidgetData*)tabviewInputWidget
{
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd || !wd->enabled || !wd->type) return nullptr;
  if (strcmp(wd->type, NEUI_W_TABVIEW) != 0) return nullptr;
  return wd;
}

- (void)mouseDown:(NSEvent*)event
{
  // TABVIEW chip click. Hit-test the cached chip rects in widget-local
  // logical px; a hit selects that tab (fires deselect/select, swaps pages).
  if (auto* tv = [self tabviewInputWidget]) {
    NSPoint p = [self localPoint:event];
    int hit = neui_detail::tabview_chip_hit(tv->tab_chips.data(),
                                             (int)tv->tab_chips.size(),
                                             (float)p.x, (float)p.y);
    if (hit >= 0) { macos_host::tabview_select_macos(*tv, hit); return; }
    [super mouseDown:event];
    return;
  }
  // SECTION scrollbar drag start. Hit-test the scrollbar gutters in
  // widget-local logical px; if the click lands in one, latch a drag.
  if (auto* sec_wd = [self sectionInputWidget]) {
    NSPoint p = [self localPoint:event];
    auto& st = *sec_wd->section_scroll_state;
    auto& L  = sec_wd->section_last_layout;
    int hit = neui_detail::section_scrollbar_hit(L, (int)p.x, (int)p.y);
    if (hit == 1) {
      st.vert_drag.active           = true;
      st.vert_drag.start_axis_coord = (int)p.y;
      st.vert_drag.start_position   = st.scroll_y;
      [self sectionStopBounce];
      return;
    }
    if (hit == 2) {
      st.horz_drag.active           = true;
      st.horz_drag.start_axis_coord = (int)p.x;
      st.horz_drag.start_position   = st.scroll_x;
      [self sectionStopBounce];
      return;
    }
    // Click in body / outside scrollbar - pass through.
  }
  if ([self isKnob]) {
    // Disabled knob ignores all pointer input (no drag, no double-click
    // reset). dragging stays false, so mouseDragged / mouseUp are inert too.
    {
      auto* wd = macos_host::widget_for_id(widget_id);
      if (wd && !wd->enabled) return;
    }
    // Double-click -> reset to NEUI_PARAM_DEFAULT.
    if (event.clickCount >= 2) {
      auto* wd = macos_host::widget_for_id(widget_id);
      float def = 0.0f;
      if (wd && wd->attrs) def = neui_clamp01(wd->attrs->get_float(NEUI_PARAM_DEFAULT, 0.0f));
      [self setKnobValueFromUser:def];
      return;
    }
    NSPoint p = [self localPointForKnobEvent:event];
    NSPoint c = [self knobCenter];
    float dx = (float)(p.x - c.x);
    float dy = (float)(p.y - c.y);
    float r2 = dx * dx + dy * dy;
    if (r2 < NEUI_KNOB_DEAD_ZONE_R * NEUI_KNOB_DEAD_ZONE_R) return;
    dragging        = true;
    drag_prev_angle = std::atan2(dy, dx);
    // Seed the continuous accumulator with the snapped current value so the
    // first delta nudges off it (rather than starting from 0).
    drag_continuous = [self knobValue];
    return;
  }
  if (auto* gwd = [self gridInputWidget]) {
    // Grab keyboard focus so arrow/page nav routes to keyDown:, then run the
    // shared grid mouse dispatch (selection ladder, scrollbar / divider drag).
    [self gridStopBounce];   // a click cancels any spring-back animation
    macos_host::ensure_grid_model_macos(*gwd).scroll_kin.suppress_momentum = false;
    [self.window makeFirstResponder:self];
    NSPoint p = [self localPoint:event];
    macos_host::GridMsg k = (event.clickCount >= 2)
      ? macos_host::GridMsg::DblClick : macos_host::GridMsg::Down;
    // Pass current modifier state in `mods` so the header-click sort logic
    // can detect Shift for multi-column sort.
    uint32_t click_mods =
      neui_detail::mac_modifiers_to_neui(event.modifierFlags);
    macos_host::grid_painted_msg_macos(*gwd, k, (float)p.x, (float)p.y,
                                         0, click_mods, 0);
    return;
  }
  if ([self customDrawWantsInput]) {
    // Grab keyboard focus so subsequent keyDown / keyUp route here (and the
    // paint pass reports focused = YES). NSView doesn't auto-focus on click.
    [self.window makeFirstResponder:self];
    if (auto* wd = macos_host::widget_for_id(widget_id); wd && !wd->pressed) {
      wd->pressed = true;
      macos_host::macos_invalidate_if_state_filtered_compound(wd);
    }
    neui_event_type_t t = (event.clickCount >= 2)
      ? NEUI_EVENT_MOUSE_BUTTON_DBLCLICK : NEUI_EVENT_MOUSE_BUTTON_DOWN;
    [self dispatchMouse:t at:[self localPoint:event] buttonmap:1];
    return;
  }
  [super mouseDown:event];
}

- (void)mouseDragged:(NSEvent*)event
{
  // SECTION scrollbar drag: continue if we latched on mouseDown.
  if (auto* sec_wd = [self sectionInputWidget]) {
    auto& st = *sec_wd->section_scroll_state;
    auto& L  = sec_wd->section_last_layout;
    if (st.vert_drag.active || st.horz_drag.active) {
      NSPoint p = [self localPoint:event];
      if (st.vert_drag.active) {
        auto geom = neui_detail::compute_scrollbar(
                       L.body_h, 0, st.content_h, L.body_h,
                       st.vert_drag.start_position);
        int new_y = neui_detail::scrollbar_drag_apply(
                       st.vert_drag, (int)p.y, geom, st.content_h, L.body_h);
        if (new_y != st.scroll_y) {
          st.scroll_y       = new_y;
          st.kinetic_over_v = false;
          macos_host::section_reposition_children_macos(*sec_wd);
          [self setNeedsDisplay:YES];
          macos_host::section_notify_scroll_changed_macos(*sec_wd);
        }
      } else {
        auto geom = neui_detail::compute_scrollbar(
                       L.body_w, 0, st.content_w, L.body_w,
                       st.horz_drag.start_position);
        int new_x = neui_detail::scrollbar_drag_apply(
                       st.horz_drag, (int)p.x, geom, st.content_w, L.body_w);
        if (new_x != st.scroll_x) {
          st.scroll_x       = new_x;
          st.kinetic_over_h = false;
          macos_host::section_reposition_children_macos(*sec_wd);
          [self setNeedsDisplay:YES];
          macos_host::section_notify_scroll_changed_macos(*sec_wd);
        }
      }
      return;
    }
  }
  if (auto* gwd = [self gridInputWidget]) {
    NSPoint p = [self localPoint:event];
    macos_host::grid_painted_msg_macos(*gwd, macos_host::GridMsg::Drag,
                                        (float)p.x, (float)p.y, 0, 0, 0);
    return;
  }
  if ([self isKnob]) {
    if (!dragging) { [super mouseDragged:event]; return; }
    NSPoint p = [self localPointForKnobEvent:event];
    NSPoint c = [self knobCenter];
    float dx = (float)(p.x - c.x);
    float dy = (float)(p.y - c.y);
    float r2 = dx * dx + dy * dy;
    if (r2 < NEUI_KNOB_DEAD_ZONE_R * NEUI_KNOB_DEAD_ZONE_R) return;  // unstable
    float cur_angle = std::atan2(dy, dx);
    float delta     = neui_knob_wrap_pi(cur_angle - drag_prev_angle);
    bool fine = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    float scale = (fine ? NEUI_KNOB_FINE_SCALE : 1.0f) / NEUI_KNOB_SWEEP_RAD;
    drag_continuous = neui_clamp01(drag_continuous + delta * scale);
    [self setKnobValueFromUser:drag_continuous];
    drag_prev_angle = cur_angle;
    return;
  }
  if ([self customDrawWantsInput]) {
    // A held-button drag surfaces as MOUSE_MOVE with NEUI_MK_LBUTTON set
    // so behavior dispatch (hosts/shared/behavior_runtime.h) can tell
    // the difference between an in-flight drag and a release-then-move.
    // Clients that don't use behaviors still receive the MOVE event as
    // before and can ignore the buttonmap field.
    [self dispatchMouse:NEUI_EVENT_MOUSE_MOVE at:[self localPoint:event] buttonmap:NEUI_MK_LBUTTON];
    return;
  }
  [super mouseDragged:event];
}

- (void)mouseUp:(NSEvent*)event
{
  // SECTION scrollbar drag release.
  if (auto* sec_wd = [self sectionInputWidget]) {
    auto& st = *sec_wd->section_scroll_state;
    if (st.vert_drag.active || st.horz_drag.active) {
      st.vert_drag.active = false;
      st.horz_drag.active = false;
      return;
    }
  }
  if (auto* gwd = [self gridInputWidget]) {
    macos_host::grid_painted_msg_macos(*gwd, macos_host::GridMsg::Up,
                                        0, 0, 0, 0, 0);
    return;
  }
  if ([self isKnob]) { dragging = false; return; }
  if ([self customDrawWantsInput]) {
    if (auto* wd = macos_host::widget_for_id(widget_id); wd && wd->pressed) {
      wd->pressed = false;
      macos_host::macos_invalidate_if_state_filtered_compound(wd);
    }
    [self dispatchMouse:NEUI_EVENT_MOUSE_BUTTON_UP at:[self localPoint:event] buttonmap:0];
    return;
  }
  [super mouseUp:event];
}

- (void)rightMouseDown:(NSEvent*)event
{
  if ([self isKnob]) {
    auto* wd = macos_host::widget_for_id(widget_id);
    if (wd && !wd->enabled) return;  // disabled knob: no context menu
    NSPoint p = [self localPointForKnobEvent:event];
    static const char* k_items[] = { "Reset to default", nullptr };
    int pick = macos_host::run_popup_menu_macos(self, (int)p.x, (int)p.y, k_items);
    if (pick == 1) {
      float def = 0.0f;
      if (wd && wd->attrs) def = neui_clamp01(wd->attrs->get_float(NEUI_PARAM_DEFAULT, 0.0f));
      [self setKnobValueFromUser:def];
    }
    return;
  }
  if ([self customDrawWantsInput]) {
    [self dispatchMouse:NEUI_EVENT_MOUSE_RBUTTON_DOWN at:[self localPoint:event] buttonmap:0];
    return;
  }
  [super rightMouseDown:event];
}
- (void)rightMouseUp:(NSEvent*)event
{
  if ([self customDrawWantsInput]) {
    [self dispatchMouse:NEUI_EVENT_MOUSE_RBUTTON_UP at:[self localPoint:event] buttonmap:0];
    return;
  }
  [super rightMouseUp:event];
}

// --- GRID smooth-scroll plumbing -------------------------------------------

// Decompose the (rubber-mapped) raw pixel scroll position into the model's
// row index + fine pixel offset, then repaint. grid_last_commit_px records
// the integer position so the next wheel event can detect external mutations
// (keyboard / drag / API) and resync.
- (void)gridStopBounce
{
  if (grid_bounce_timer) { [grid_bounce_timer invalidate]; grid_bounce_timer = nil; }
}

- (void)gridStartBounce
{
  [self gridStopBounce];
  grid_bounce_timer = [NSTimer timerWithTimeInterval:1.0 / 60.0
                                              target:self
                                            selector:@selector(gridBounceTick:)
                                            userInfo:nil
                                             repeats:YES];
  // Common modes so the spring-back keeps animating during event tracking.
  [[NSRunLoop currentRunLoop] addTimer:grid_bounce_timer
                               forMode:NSRunLoopCommonModes];
}

- (void)gridBounceTick:(NSTimer*)timer
{
  (void)timer;
  auto* wd = [self gridInputWidget];
  if (!wd) { [self gridStopBounce]; return; }
  auto& m   = macos_host::ensure_grid_model_macos(*wd);
  auto  cfg = neui_detail::grid_read_config(wd->attrs.get());
  auto  vp  = macos_host::grid_viewport_macos(*wd);
  bool more = neui_detail::grid_scroll_bounce_step(m, vp, cfg.row_h);
  [self setNeedsDisplay:YES];
  if (!more) [self gridStopBounce];
}

// --- Scrolling SECTION smooth-scroll plumbing ------------------------------

// Stop / start the 60 Hz spring-back timer driving the rubber-band release.
- (void)sectionStopBounce
{
  if (section_bounce_timer) { [section_bounce_timer invalidate]; section_bounce_timer = nil; }
}

- (void)sectionStartBounce
{
  [self sectionStopBounce];
  section_bounce_timer = [NSTimer timerWithTimeInterval:1.0 / 60.0
                                                 target:self
                                               selector:@selector(sectionBounceTick:)
                                               userInfo:nil
                                                repeats:YES];
  [[NSRunLoop currentRunLoop] addTimer:section_bounce_timer
                               forMode:NSRunLoopCommonModes];
}

- (void)sectionBounceTick:(NSTimer*)timer
{
  (void)timer;
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd || !wd->section_scroll_state) { [self sectionStopBounce]; return; }
  bool more = macos_host::section_bounce_step_macos(*wd);
  [self setNeedsDisplay:YES];
  macos_host::section_notify_scroll_changed_macos(*wd);
  if (!more) [self sectionStopBounce];
}

// SECTION wheel - rich phase / momentum / precise-delta plumbing per axis.
// Mirror of gridScrollWheel:, feeding section_scroll_wheel_kinetic via
// the shared helper that also handles asymmetric single-axis fallback.
- (void)sectionScrollWheel:(NSEvent*)event widget:(macos_host::WidgetData*)wd
{
  using namespace neui_detail;
  if (!wd->section_scroll_state) return;
  auto& st = *wd->section_scroll_state;
  auto& L  = wd->section_last_layout;

  bool has_v = section_axis_has_v(st.axis);
  bool has_h = section_axis_has_h(st.axis);

  // Build the per-axis ScrollWheelInput. precise => trackpad / Magic Mouse
  // (deltas in points); classic mouse-wheel deltas arrive in lines which
  // we scale by SECTION_WHEEL_LINE_PX so the visual speed matches the
  // kinetic-precise path.
  bool precise = event.hasPreciseScrollingDeltas;
  double dy = (double)event.scrollingDeltaY;
  double dx = (double)event.scrollingDeltaX;
  if (!precise) {
    dy *= (double)SECTION_WHEEL_LINE_PX;
    dx *= (double)SECTION_WHEEL_LINE_PX;
  }
  // Asymmetric single-axis fallback (mirror of the win32 + xpl shape):
  // a horizontal-only section absorbs a pure vertical wheel because
  // classic mouse wheels have no horizontal axis. A vertical-only
  // section does NOT absorb pure horizontal input - the user explicitly
  // requested horizontal motion (two-finger sideways trackpad).
  if (!has_v && has_h && dx == 0.0 && dy != 0.0) { dx = dy; dy = 0.0; }
  // Cross-axis input that the section's axis mask doesn't support is
  // dropped at the per-axis check below.

  // NEUI_ATTR_SCROLL_KINETICS opt-in: STEPPED suppresses rubber-band +
  // momentum stream; PLATFORM on macOS = SMOOTH (the natural trackpad feel).
  int  kin_mode = section_read_kinetics_mode(wd->attrs.get());
  bool smooth   = scroll_kinetics_smooth_enabled(kin_mode,
                                                  /*platform_default_smooth=*/true);
  if (!smooth) {
    // Drop momentum events so a flick doesn't keep stepping after release.
    if (event.momentumPhase != NSEventPhaseNone) return;
    bool changed = false;
    if (has_v && dy != 0.0 && section_scroll_step_px(st, L, dy, false))
      changed = true;
    if (has_h && dx != 0.0 && section_scroll_step_px(st, L, dx, true))
      changed = true;
    if (changed) {
      [self sectionStopBounce];
      macos_host::section_reposition_children_macos(*wd);
      [self setNeedsDisplay:YES];
      macos_host::section_notify_scroll_changed_macos(*wd);
    }
    return;
  }

  ScrollWheelInput base;
  base.precise        = precise;
  base.phase_began    = (event.phase == NSEventPhaseBegan);
  base.phase_changed  = (event.phase == NSEventPhaseChanged);
  base.phase_ended    = (event.phase == NSEventPhaseEnded) ||
                        (event.phase == NSEventPhaseCancelled);
  base.momentum       = (event.momentumPhase != NSEventPhaseNone);
  base.momentum_ended = (event.momentumPhase == NSEventPhaseEnded);

  // Zero-delta events still matter when they carry gesture-phase edges
  // (phase_began cancels a bounce; phase_ended / momentum_ended drive the
  // spring-back + momentum-suppression bookkeeping). Same shape as the xpl
  // host's sectionKineticWheel:.
  bool phase_signal = base.phase_began || base.phase_ended ||
                      base.momentum || base.momentum_ended;

  ScrollWheelAction act_v{}, act_h{};
  if (has_v && (dy != 0.0 || phase_signal)) {
    ScrollWheelInput in = base;
    in.delta_px = dy;
    act_v = section_scroll_wheel_kinetic(st, L, in, false);
  }
  if (has_h && (dx != 0.0 || phase_signal)) {
    ScrollWheelInput in = base;
    // Horizontal axis: scrollingDeltaX matches scrollingDeltaY's natural-
    // scroll convention (positive = content moves right = scroll_x
    // decreases). raw_px -= delta_px in the kinetics, so we pass dx
    // through unchanged - same as the xpl host's sectionKineticWheel:.
    in.delta_px = dx;
    act_h = section_scroll_wheel_kinetic(st, L, in, true);
  }
  if (act_v.stop_bounce || act_h.stop_bounce) [self sectionStopBounce];
  if (act_v.changed || act_h.changed) {
    macos_host::section_reposition_children_macos(*wd);
    [self setNeedsDisplay:YES];
    macos_host::section_notify_scroll_changed_macos(*wd);
  }
  if (act_v.start_bounce || act_h.start_bounce) [self sectionStartBounce];
}

- (void)gridScrollWheel:(NSEvent*)event widget:(macos_host::WidgetData*)wd
{
  auto& m   = macos_host::ensure_grid_model_macos(*wd);
  auto  cfg = neui_detail::grid_read_config(wd->attrs.get());
  auto  vp  = macos_host::grid_viewport_macos(*wd);

  // macOS default = SMOOTH (the natural trackpad feel); client can flip the
  // attr to STEPPED for a Win32-like coarse row scroll.
  if (!neui_detail::grid_smooth_enabled(cfg, /*platform_default_smooth=*/true)) {
    // Drop momentum events so a flick doesn't keep stepping after release.
    if (event.momentumPhase != NSEventPhaseNone) return;
    CGFloat raw = event.scrollingDeltaY;
    if (raw == 0) return;
    // Match the knob / customdraw accumulator: precise deltas in points -> tick;
    // classic mouse wheel already arrives ~1.0 per notch.
    CGFloat scaled = event.hasPreciseScrollingDeltas
      ? (raw / NEUI_WHEEL_PRECISE_POINTS_PER_TICK)
      : raw;
    wheel_accum_y += scaled;
    int ticks = (int)wheel_accum_y;
    if (ticks == 0) return;
    wheel_accum_y -= (CGFloat)ticks;
    // Wheel-up (positive scrollingDeltaY) = content moves down = scroll fewer
    // rows = negative row_step. Mirror the SMOOTH path's natural-scroll sign.
    [self gridStopBounce];
    if (neui_detail::grid_scroll_step_rows(m, vp, cfg.row_h, -ticks))
      [self setNeedsDisplay:YES];
    return;
  }

  neui_detail::GridWheelInput in;
  // Precise (trackpad / Magic Mouse) deltas are in points; legacy mouse-wheel
  // deltas are in lines (one line == one row).
  in.precise        = event.hasPreciseScrollingDeltas;
  double dy         = (double)event.scrollingDeltaY;
  in.delta_px       = in.precise ? dy : dy * (double)(cfg.row_h > 0 ? cfg.row_h : 1);
  in.phase_began    = (event.phase == NSEventPhaseBegan);
  in.phase_changed  = (event.phase == NSEventPhaseChanged);
  in.phase_ended    = (event.phase == NSEventPhaseEnded) ||
                      (event.phase == NSEventPhaseCancelled);
  in.momentum       = (event.momentumPhase != NSEventPhaseNone);
  in.momentum_ended = (event.momentumPhase == NSEventPhaseEnded);

  neui_detail::GridWheelAction act = neui_detail::grid_scroll_wheel(m, vp, cfg.row_h, in);
  if (act.stop_bounce)  [self gridStopBounce];
  if (act.changed)      [self setNeedsDisplay:YES];
  if (act.start_bounce) [self gridStartBounce];
}

- (void)scrollWheel:(NSEvent*)event
{
  bool wants_knob = [self isKnob];
  macos_host::WidgetData* grid_wd = wants_knob ? nullptr : [self gridInputWidget];
  bool wants_grid = (grid_wd != nullptr);
  // SECTION scrolling: the painted view's widget is a SECTION with an
  // allocated SectionScrollState. Sections are otherwise non-interactive.
  macos_host::WidgetData* sec_wd = nullptr;
  if (!wants_knob && !wants_grid) {
    auto* wd = macos_host::widget_for_id(widget_id);
    if (wd && macos_host::is_section_like(wd->type)
        && wd->section_scroll_state)
      sec_wd = wd;
  }
  bool wants_section = (sec_wd != nullptr);
  bool wants_cd   = !wants_knob && !wants_grid && !wants_section &&
                     [self customDrawWantsInput];
  if (!wants_knob && !wants_grid && !wants_section && !wants_cd) {
    [super scrollWheel:event]; return;
  }

  // GRID gets pixel-precise smooth scrolling with inertial momentum + elastic
  // rubber-band - it consumes momentum events rather than dropping them, so it
  // must run before the knob / customdraw tick model below.
  if (wants_grid) {
    [self gridScrollWheel:event widget:grid_wd];
    return;
  }

  // SECTION shares the same shape - rich NSEvent phase / momentum / precise
  // deltas feed section_scroll_wheel_kinetic per axis.
  if (wants_section) {
    [self sectionScrollWheel:event widget:sec_wd];
    return;
  }

  // Drop momentum-phase events. Inertial follow-through after a flick keeps
  // streaming for hundreds of ms; on a parameter knob that feels like the
  // value is still moving after the user let go. AppKit emits momentum events
  // with momentumPhase != NSEventPhaseNone.
  if (event.momentumPhase != NSEventPhaseNone) return;

  CGFloat raw = event.scrollingDeltaY;
  if (raw == 0) return;

  // Normalize input to integer ticks (Win32 WHEEL_DELTA model). Traditional
  // mouse wheels send ~1.0 per notch already; precise (trackpad / Magic
  // Mouse) deltas arrive in points, many events per swipe, so divide before
  // accumulating. Fractional remainder persists across events so slow swipes
  // still register.
  CGFloat scaled = event.hasPreciseScrollingDeltas
    ? (raw / NEUI_WHEEL_PRECISE_POINTS_PER_TICK)
    : raw;
  wheel_accum_y += scaled;
  int ticks = (int)wheel_accum_y;  // truncates toward zero
  if (ticks == 0) return;
  wheel_accum_y -= (CGFloat)ticks;

  if (wants_knob) {
    auto* wd = macos_host::widget_for_id(widget_id);
    if (wd && !wd->enabled) return;  // disabled knob ignores the wheel
    // Match the xpl host's wheel-up = increase convention (matches the slider).
    bool fine = (event.modifierFlags & NSEventModifierFlagShift) != 0;
    int steps = [self knobSteps];
    float magnitude = (steps >= 2)
      ? (1.0f / (float)(steps - 1))
      : (fine ? 0.01f : 0.05f);
    float sign = (ticks > 0) ? 1.0f : -1.0f;
    int   mag_ticks = (ticks > 0) ? ticks : -ticks;
    [self setKnobValueFromUser:[self knobValue]
                                + sign * magnitude * (float)mag_ticks];
    return;
  }

  // CUSTOMDRAW: forward as a single MOUSE_WHEEL event with delta=ticks (the
  // behavior runtime multiplies by |delta|, matching Win32 line-count semantics).
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess) return;
  NSPoint p = [self localPoint:event];
  neui_event_t ev = {};
  ev.type              = NEUI_EVENT_MOUSE_WHEEL;
  ev.data.wheel.widget = { wd->widget_id };
  ev.data.wheel.x      = (int)p.x;
  ev.data.wheel.y      = (int)p.y;
  ev.data.wheel.delta  = ticks;
  sess->dispatch_event(&ev);
  macos_host::dispatch_behavior_mouse(widget_id, &ev, (float)p.x, (float)p.y);
}

@end

// ---------------------------------------------------------------------------
// NEUINativeWindowDelegate - close button + quit-on-last-appwindow.
// (Interface with ivars declared near the top of this file so the
// content view's <NSDraggingDestination> methods can see the layout.)

@implementation NEUINativeWindowDelegate

- (BOOL)windowShouldClose:(NSWindow*)sender
{
  if (!session) return YES;

  // Give the client a chance to veto. APP_QUIT is the veto event for any
  // frame close (mirrors the win32 / xpl host's WM_CLOSE -> APP_QUIT path);
  // dialogs and appwindows go through the same dispatch.
  neui_event_t ev = {};
  ev.type = NEUI_EVENT_APP_QUIT;
  if (!session->dispatch_event(&ev)) return NO;

  // For a modal sheet, returning YES would cause AppKit to call -close on
  // the sheet without ending the sheet session - the parent stays disabled
  // and the next beginSheet: would assert. Drive endSheet: ourselves; the
  // completion handler installed in create_dialog closes the sheet's
  // window after detachment.
  if (sheet_active && sheet_owner) {
    sheet_active = false;
    [sheet_owner endSheet:sender];
    return NO;
  }
  return YES;
}

- (void)windowWillClose:(NSNotification*)note
{
  (void)note;
  if (handled_close) return;
  handled_close = true;
  // Drop the modal pump flag so the nested NSEvent loop in widget_show
  // unwinds and returns to the caller. No-op for non-modal frames.
  if (session && session->_widgets.exists(widget_index)) {
    auto& wd = session->_widgets[widget_index];
    if (wd.modal_pump_active) wd.modal_pump_active = false;
  }
  if (is_appwindow) {
    if (--g_appwindow_count <= 0) {
      [NSApp stop:nil];
      wake_app_event_pump();
    }
  }
}

// Frame focus -> NEUI_EVENT_WIDGET_FOCUS for the frame widget. Mirror of the
// win32 host's WM_SETFOCUS / WM_KILLFOCUS path. Clients see logical focus at
// the frame granularity (Tier B per-widget focus proxies are deferred).
- (void)dispatchFrameFocus:(bool)gained
{
  if (!session || !session->_widgets.exists(widget_index)) return;
  neui_event_t ev = {};
  ev.type              = NEUI_EVENT_WIDGET_FOCUS;
  ev.data.focus.widget = { session->_widgets[widget_index].widget_id };
  ev.data.focus.focused = gained;
  session->dispatch_event(&ev);
}

- (void)windowDidBecomeKey:(NSNotification*)note { (void)note; [self dispatchFrameFocus:true];  }
- (void)windowDidResignKey:(NSNotification*)note { (void)note; [self dispatchFrameFocus:false]; }

// Frame resize -> NEUI_EVENT_RESIZE with the new content size in logical
// pixels. Mirror of the win32 host's WM_SIZE path. The content view is
// isFlipped with a backing-scale CTM, so contentView.bounds is already in
// logical points (= logical px at 96 DPI).
- (void)windowDidResize:(NSNotification*)note
{
  if (!session || !session->_widgets.exists(widget_index)) return;
  NSWindow* win = (NSWindow*)note.object;
  if (![win isKindOfClass:[NSWindow class]]) return;
  NSSize sz = win.contentView ? win.contentView.bounds.size : win.frame.size;

  auto& wd = session->_widgets[widget_index];
  wd.width  = (int)sz.width;
  wd.height = (int)sz.height;

  neui_event_t ev = {};
  ev.type               = NEUI_EVENT_RESIZE;
  ev.data.resize.widget = { wd.widget_id };
  ev.data.resize.width  = (int)sz.width;
  ev.data.resize.height = (int)sz.height;
  session->dispatch_event(&ev);
}

@end

// ---------------------------------------------------------------------------
// NEUINativeControlTarget - singleton action sink for NSControls. Each
// NSControl's tag = widget_id ((session_id<<16) | tree_index). The selector
// looks up the session in macos_host::sessions and dispatches the
// appropriate neui_event_t.

@interface NEUINativeControlTarget : NSObject
+ (instancetype)shared;
- (void)neuiControlAction:(id)sender;
@end

@implementation NEUINativeControlTarget

+ (instancetype)shared
{
  static NEUINativeControlTarget* s = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ s = [[NEUINativeControlTarget alloc] init]; });
  return s;
}

- (void)neuiControlAction:(id)sender
{
  if (![sender isKindOfClass:[NSControl class]]) return;
  NSControl* c = (NSControl*)sender;
  uint32_t widget_id  = (uint32_t)c.tag;
  uint32_t session_id = (widget_id >> 16) & 0xffff;
  uint32_t idx        = widget_id & 0xffff;
  if (session_id == 0) return;
  size_t sess_idx = static_cast<size_t>(session_id) - 1;
  if (sess_idx >= macos_host::sessions.size()) return;
  auto& sess_ptr = macos_host::sessions[sess_idx];
  if (!sess_ptr) return;
  auto* sess = sess_ptr.get();
  if (!sess->_widgets.exists(idx)) return;
  auto& wd = sess->_widgets[idx];
  if (!wd.emit_events) return;

  if (!wd.type) return;
  if (!strcmp(wd.type, NEUI_W_BUTTON)) {
    neui_event_t ev = {};
    ev.type                 = NEUI_EVENT_MOUSE_BUTTON_CLICK;
    ev.data.mouse.widget    = { wd.widget_id };
    ev.data.mouse.buttonmap = 0;
    sess->dispatch_event(&ev);
    return;
  }
  if (!strcmp(wd.type, NEUI_W_CHECKBOX) || !strcmp(wd.type, NEUI_W_CHECKBOX3)) {
    NSButton* btn = (NSButton*)c;

    // The button is NSButtonTypeMomentaryChange (no auto-toggle), so we
    // own the entire state machine: read cached state, advance by 2 or 3
    // depending on the tristate attr, swap the SF Symbol image, write
    // back. NSButton.state is never read or written - we never want
    // AppKit to render its checkbox cell here.
    bool tristate = wd.attrs && wd.attrs->get_int(NEUI_ATTR_TRISTATE, 0) != 0;
    int prev = wd.attrs ? wd.attrs->get_int("neui.macoshost.checkstate",
                                              NEUI_CHECK_UNCHECKED)
                        : NEUI_CHECK_UNCHECKED;
    int mod  = tristate ? 3 : 2;
    int next = (prev + 1) % mod;
    btn.image = macos_host::checkbox_image_for_state(next);
    neui_detail::ensure_attrs(wd.attrs).set_int("neui.macoshost.checkstate", next);

    neui_event_t ev = {};
    ev.type                 = NEUI_EVENT_CHECKBOX_CHANGED;
    ev.data.checkbox.widget = { wd.widget_id };
    ev.data.checkbox.state  = (neui_check_state_t)next;
    sess->dispatch_event(&ev);
    return;
  }
  if (!strcmp(wd.type, NEUI_W_COMBOBOX)) {
    NSPopUpButton* pb = (NSPopUpButton*)c;
    NSInteger idx = pb.indexOfSelectedItem;
    uint32_t selected = (idx >= 0 && (size_t)idx < wd.items.size())
                         ? (uint32_t)idx : NEUI_ITEM_NONE;
    wd.selected_item = selected;
    neui_event_t ev = {};
    ev.type             = NEUI_EVENT_ITEM_SELECTED;
    ev.data.item.widget = { wd.widget_id };
    ev.data.item.index  = selected;
    sess->dispatch_event(&ev);
    return;
  }
  if (!strcmp(wd.type, NEUI_W_SLIDER)) {
    NSSlider* sl = (NSSlider*)c;
    float v = (float)sl.doubleValue;
    if (v < 0) v = 0; if (v > 1) v = 1;
    if (wd.attrs) wd.attrs->set_float(NEUI_PARAM_VALUE, v);
    neui_event_t ev = {};
    ev.type              = NEUI_EVENT_VALUE_CHANGED;
    ev.data.value.widget = { wd.widget_id };
    ev.data.value.value  = v;
    sess->dispatch_event(&ev);
    return;
  }
}

@end

// ---------------------------------------------------------------------------
// NEUINativeTextDelegate - singleton sink for NSTextField / NSTextView change
// notifications. Looks up the changed control's owning widget via tag (set
// by create_inputbox / create_multiline) and fires NEUI_EVENT_WIDGET_UPDATED.

namespace macos_host {
  // Helper used by the delegate to dispatch a WIDGET_UPDATED event without
  // duplicating the (session_id<<16 | tree_idx) decoding.
  static void dispatch_widget_updated(uint32_t widget_id)
  {
    uint32_t session_id = (widget_id >> 16) & 0xffff;
    uint32_t idx        = widget_id & 0xffff;
    if (session_id == 0) return;
    size_t sess_idx = static_cast<size_t>(session_id) - 1;
    if (sess_idx >= sessions.size()) return;
    auto& sp = sessions[sess_idx];
    if (!sp) return;
    auto* sess = sp.get();
    if (!sess->_widgets.exists(idx)) return;
    auto& wd = sess->_widgets[idx];
    if (!wd.emit_events) return;
    neui_event_t ev = {};
    ev.type = NEUI_EVENT_WIDGET_UPDATED;
    sess->dispatch_event(&ev);
  }
}

// ---------------------------------------------------------------------------
// NEUINativeListSource - per-LISTBOX NSTableViewDataSource + Delegate. Holds
// the owning widget_id so reads / selection events can route back to the
// session. One source instance per LISTBOX widget; the NSScrollView owns a
// strong reference via objc_setAssociatedObject.

@interface NEUINativeListSource : NSObject<NSTableViewDataSource, NSTableViewDelegate>
{
@public
  uint32_t widget_id;
}
@end

namespace macos_host {
  // Helper to look up a Session + WidgetData by widget_id (decodes the
  // upper 16 bits as session_id, lower 16 as tree slot). Returns null on
  // any lookup miss. Non-static so NEUINativePaintedView (defined earlier
  // in this TU) can reference via the forward declaration at file top.
  WidgetData* widget_for_id(uint32_t widget_id, Session** out_session)
  {
    uint32_t session_id = (widget_id >> 16) & 0xffff;
    uint32_t idx        = widget_id & 0xffff;
    if (session_id == 0) return nullptr;
    size_t sess_idx = static_cast<size_t>(session_id) - 1;
    if (sess_idx >= sessions.size()) return nullptr;
    auto& sp = sessions[sess_idx];
    if (!sp) return nullptr;
    auto* sess = sp.get();
    if (!sess->_widgets.exists(idx)) return nullptr;
    if (out_session) *out_session = sess;
    return &sess->_widgets[idx];
  }

  // Painter draw_asset thunk - resolves a neui_asset_t through the
  // owning session's MacOSAssetManager, lazy-uploads a CGImage on first
  // use per (asset, ctx) pair, then calls backend->draw_bitmap. Mirror of
  // hosts/win32/widgets.cpp::w32_painter_draw_asset_thunk - CG's
  // get_context_generation is a constant so the generation comparison
  // is a no-op here, but the shape is kept for symmetry.
  void NEUI_ABI macos_painter_draw_asset_thunk(void* host_token,
                                                 neui_render_backend_t* backend,
                                                 neui_render_ctx_t ctx,
                                                 neui_asset_t asset,
                                                 float x, float y,
                                                 float w, float h,
                                                 uint32_t tint)
  {
    auto* s = static_cast<Session*>(host_token);
    if (!s || !backend || !ctx) return;
    if (asset.id == asset_none.id) return;
    if (((asset.id >> 16) & 0xffff) != (s->session_id() & 0xffff)) return;
    uint32_t slot = asset.id & 0xffff;
    auto* entry = s->_asset_manager.get_slot(slot);
    if (!entry) return;
    // Cache-walk + lazy GPU upload + draw shared with the other hosts
    // (hosts/shared/painter.h).
    neui_detail::painter_draw_entry_cached(backend, ctx, entry,
                                            x, y, w, h, tint);
  }
}

@implementation NEUINativeListSource

- (NSInteger)numberOfRowsInTableView:(NSTableView*)tableView
{
  (void)tableView;
  auto* wd = macos_host::widget_for_id(widget_id);
  return wd ? (NSInteger)wd->items.size() : 0;
}

- (NSView*)tableView:(NSTableView*)tableView
   viewForTableColumn:(NSTableColumn*)tableColumn
                  row:(NSInteger)row
{
  (void)tableColumn;
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd || row < 0 || (size_t)row >= wd->items.size()) return nil;

  static NSString* const k_id = @"neui.listcell";
  NSTableCellView* cv = [tableView makeViewWithIdentifier:k_id owner:self];
  if (!cv) {
    cv = [[NSTableCellView alloc] initWithFrame:NSZeroRect];
    cv.identifier = k_id;
    NSTextField* tf = [NSTextField labelWithString:@""];
    tf.translatesAutoresizingMaskIntoConstraints = NO;
    [cv addSubview:tf];
    cv.textField = tf;
    [NSLayoutConstraint activateConstraints:@[
      [tf.leadingAnchor  constraintEqualToAnchor:cv.leadingAnchor  constant:4],
      [tf.trailingAnchor constraintEqualToAnchor:cv.trailingAnchor constant:-4],
      [tf.centerYAnchor  constraintEqualToAnchor:cv.centerYAnchor],
    ]];
  }
  cv.textField.stringValue =
    [NSString stringWithUTF8String:wd->items[(size_t)row].text.c_str()];
  return cv;
}

- (void)tableViewSelectionDidChange:(NSNotification*)note
{
  NSTableView* tv = (NSTableView*)note.object;
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess || !wd->emit_events) return;
  NSInteger row = tv.selectedRow;
  uint32_t idx = (row >= 0 && (size_t)row < wd->items.size())
                  ? (uint32_t)row : NEUI_ITEM_NONE;
  wd->selected_item = idx;
  neui_event_t ev = {};
  ev.type             = NEUI_EVENT_ITEM_SELECTED;
  ev.data.item.widget = { wd->widget_id };
  ev.data.item.index  = idx;
  sess->dispatch_event(&ev);
}

@end

// ---------------------------------------------------------------------------
// NEUIPopupCollector - records the picked NSMenuItem's tag for the blocking
// popup_menu helper. Each item's action targets this object; after the
// nested popup tracking loop returns, pickedTag holds the 1-based choice
// (or 0 if the menu was dismissed without a pick).

@interface NEUIPopupCollector : NSObject
{
@public
  int pickedTag;
}
- (void)neuiPopupPick:(id)sender;
@end

@implementation NEUIPopupCollector
- (instancetype)init { if ((self = [super init])) pickedTag = 0; return self; }
- (void)neuiPopupPick:(id)sender
{
  NSMenuItem* mi = (NSMenuItem*)sender;
  if ([mi isKindOfClass:[NSMenuItem class]]) pickedTag = (int)mi.tag;
}
@end

namespace macos_host {
  int run_popup_menu_macos(NSView* anchor, int x, int y, const char* const* items)
  {
    if (!items) return 0;
    NSMenu* menu = [[NSMenu alloc] initWithTitle:@""];
    menu.autoenablesItems = NO;
    NEUIPopupCollector* collector = [[NEUIPopupCollector alloc] init];

    int idx = 0;
    for (const char* const* p = items; *p; ++p) {
      ++idx;  // separators consume an index slot (matches the win32 host)
      const char* t = *p;
      if (t[0] == '-' && t[1] == '\0') {
        [menu addItem:[NSMenuItem separatorItem]];
        continue;
      }
      NSMenuItem* mi = [[NSMenuItem alloc]
        initWithTitle:[NSString stringWithUTF8String:t]
               action:@selector(neuiPopupPick:)
        keyEquivalent:@""];
      mi.target  = collector;
      mi.tag     = idx;
      mi.enabled = YES;
      [menu addItem:mi];
    }

    if (anchor) {
      [menu popUpMenuPositioningItem:nil
                          atLocation:NSMakePoint(x, y)
                              inView:anchor];
    } else {
      NSWindow* kw = [NSApp keyWindow];
      if (kw.contentView)
        [menu popUpMenuPositioningItem:nil
                            atLocation:NSMakePoint(x, y)
                                inView:kw.contentView];
    }
    return collector->pickedTag;
  }
}

// ---------------------------------------------------------------------------
// NEUINativeMenuTarget - singleton sink for menu-item picks. Decodes
// (widget_id, item_id) from the NSMenuItem's representedObject + tag, looks
// up the macos_host::Session, fires NEUI_EVENT_TREE_ITEM_ACTIVATED.

@interface NEUINativeMenuTarget : NSObject
+ (instancetype)shared;
- (void)neuiNativeMenuPick:(id)sender;
@end

@implementation NEUINativeMenuTarget

+ (instancetype)shared
{
  static NEUINativeMenuTarget* s = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ s = [[NEUINativeMenuTarget alloc] init]; });
  return s;
}

- (void)neuiNativeMenuPick:(id)sender
{
  NSMenuItem* it = (NSMenuItem*)sender;
  if (![it isKindOfClass:[NSMenuItem class]]) return;
  NSNumber* widget_id_num = (NSNumber*)it.representedObject;
  if (!widget_id_num) return;
  uint32_t widget_id = widget_id_num.unsignedIntValue;
  uint32_t item_id   = (uint32_t)it.tag;

  uint32_t session_id = (widget_id >> 16) & 0xffff;
  uint32_t idx        = widget_id & 0xffff;
  if (session_id == 0) return;
  size_t sess_idx = static_cast<size_t>(session_id) - 1;
  if (sess_idx >= macos_host::sessions.size()) return;
  auto& sp = macos_host::sessions[sess_idx];
  if (!sp) return;
  auto* sess = sp.get();
  if (!sess->_widgets.exists(idx)) return;
  auto& wd = sess->_widgets[idx];

  // Routed-command binding (tree->set_menu_cmd): a built-in command goes to
  // the focused widget first. If it's consumed there, the client does NOT see
  // TREE_ITEM_ACTIVATED. Mirror of the win32 host's dispatch_menu_event.
  auto cmd_it = wd.tree_items.find(item_id);
  if (cmd_it != wd.tree_items.end()) {
    uint32_t cmd = cmd_it->second.menu_cmd;
    if (cmd != 0 && cmd < NEUI_CMD_USER_BASE &&
        macos_host::invoke_focused_command_macos(cmd)) {
      return;  // consumed by the focused widget
    }
  }

  neui_event_t ev = {};
  ev.type             = NEUI_EVENT_TREE_ITEM_ACTIVATED;
  ev.data.tree.widget = { wd.widget_id };
  ev.data.tree.item   = { item_id };
  sess->dispatch_event(&ev);
}

@end

// ---------------------------------------------------------------------------
// NEUINativeOutlineSource - per-TREEVIEW NSOutlineViewDataSource + Delegate.
// Items are NSNumber-wrapped neui tree_ids; child:ofItem: walks the
// tree_items_ordered vector filtering by parent_id so children appear in
// insertion order (matches the xpl host / win32 host's behaviour).

@interface NEUINativeOutlineSource : NSObject<NSOutlineViewDataSource, NSOutlineViewDelegate>
{
@public
  uint32_t widget_id;
}
@end

namespace macos_host {
  // Walk tree_items_ordered, return the n-th item whose parent_id matches.
  // Used by NEUINativeOutlineSource's child:ofItem: + numberOfChildrenOfItem:.
  static uint32_t tree_nth_child(const WidgetData& wd, uint32_t parent_id, NSInteger n)
  {
    NSInteger seen = 0;
    for (uint32_t id : wd.tree_items_ordered) {
      auto it = wd.tree_items.find(id);
      if (it == wd.tree_items.end()) continue;
      if (it->second.parent_id != parent_id) continue;
      if (seen == n) return id;
      ++seen;
    }
    return 0;
  }

  static NSInteger tree_count_children(const WidgetData& wd, uint32_t parent_id)
  {
    NSInteger c = 0;
    for (uint32_t id : wd.tree_items_ordered) {
      auto it = wd.tree_items.find(id);
      if (it == wd.tree_items.end()) continue;
      if (it->second.parent_id == parent_id) ++c;
    }
    return c;
  }
}

@implementation NEUINativeOutlineSource

- (NSInteger)outlineView:(NSOutlineView*)outlineView numberOfChildrenOfItem:(id)item
{
  (void)outlineView;
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd) return 0;
  uint32_t parent_id = item ? (uint32_t)((NSNumber*)item).unsignedIntValue : 0;
  return macos_host::tree_count_children(*wd, parent_id);
}

- (id)outlineView:(NSOutlineView*)outlineView child:(NSInteger)index ofItem:(id)item
{
  (void)outlineView;
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd) return nil;
  uint32_t parent_id = item ? (uint32_t)((NSNumber*)item).unsignedIntValue : 0;
  uint32_t cid = macos_host::tree_nth_child(*wd, parent_id, index);
  return cid ? @(cid) : nil;
}

- (BOOL)outlineView:(NSOutlineView*)outlineView isItemExpandable:(id)item
{
  (void)outlineView;
  if (!item) return YES;
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd) return NO;
  uint32_t id_v = (uint32_t)((NSNumber*)item).unsignedIntValue;
  return macos_host::tree_count_children(*wd, id_v) > 0;
}

- (id)outlineView:(NSOutlineView*)outlineView
       objectValueForTableColumn:(NSTableColumn*)tableColumn
                          byItem:(id)item
{
  (void)outlineView; (void)tableColumn;
  if (!item) return @"";
  auto* wd = macos_host::widget_for_id(widget_id);
  if (!wd) return @"";
  uint32_t id_v = (uint32_t)((NSNumber*)item).unsignedIntValue;
  auto it = wd->tree_items.find(id_v);
  if (it == wd->tree_items.end()) return @"";
  return [NSString stringWithUTF8String:it->second.text.c_str()];
}

// View-based row rendering. Required on macOS 26 / Liquid Glass - cell-based
// NSOutlineView misaligns the disclosure chevron (it sits below the text
// baseline) because the legacy cell vertical-centre math wasn't updated for
// the new row metrics. View-based mode lays the chevron out against the
// NSTableCellView's textField bounds correctly.
- (NSView*)outlineView:(NSOutlineView*)outlineView
    viewForTableColumn:(NSTableColumn*)tableColumn
                  item:(id)item
{
  (void)tableColumn;
  NSTableCellView* cell = [outlineView makeViewWithIdentifier:@"NEUITreeCell" owner:self];
  if (!cell) {
    cell = [[NSTableCellView alloc] initWithFrame:NSMakeRect(0, 0, 100, 20)];
    cell.identifier = @"NEUITreeCell";
    NSTextField* tf = [NSTextField labelWithString:@""];
    tf.translatesAutoresizingMaskIntoConstraints = NO;
    tf.drawsBackground = NO;
    tf.editable        = NO;
    tf.selectable      = NO;
    tf.bezeled         = NO;
    tf.lineBreakMode   = NSLineBreakByTruncatingTail;
    [cell addSubview:tf];
    cell.textField = tf;
    [NSLayoutConstraint activateConstraints:@[
      [tf.leadingAnchor  constraintEqualToAnchor:cell.leadingAnchor],
      [tf.trailingAnchor constraintEqualToAnchor:cell.trailingAnchor],
      [tf.centerYAnchor  constraintEqualToAnchor:cell.centerYAnchor],
    ]];
  }
  NSString* text = @"";
  if (item) {
    auto* wd = macos_host::widget_for_id(widget_id);
    if (wd) {
      uint32_t id_v = (uint32_t)((NSNumber*)item).unsignedIntValue;
      auto it = wd->tree_items.find(id_v);
      if (it != wd->tree_items.end())
        text = [NSString stringWithUTF8String:it->second.text.c_str()];
    }
  }
  cell.textField.stringValue = text;
  return cell;
}

- (void)outlineViewSelectionDidChange:(NSNotification*)note
{
  NSOutlineView* ov = (NSOutlineView*)note.object;
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess) return;
  NSInteger row = ov.selectedRow;
  uint32_t id_v = 0;
  if (row >= 0) {
    id selectedItem = [ov itemAtRow:row];
    if (selectedItem) id_v = (uint32_t)((NSNumber*)selectedItem).unsignedIntValue;
  }
  wd->selected_tree_item = id_v ? id_v : UINT32_MAX;
  if (!wd->emit_events) return;
  neui_event_t ev = {};
  ev.type             = NEUI_EVENT_TREE_ITEM_SELECTED;
  ev.data.tree.widget = { wd->widget_id };
  ev.data.tree.item   = { id_v };
  sess->dispatch_event(&ev);
}

// Double-click -> TREE_ITEM_ACTIVATED. Wired via the table view's
// doubleAction in create_treeview.
- (void)neuiOutlineDoubleClick:(id)sender
{
  NSOutlineView* ov = (NSOutlineView*)sender;
  macos_host::Session* sess = nullptr;
  auto* wd = macos_host::widget_for_id(widget_id, &sess);
  if (!wd || !sess) return;
  NSInteger row = ov.clickedRow;
  if (row < 0) return;
  id item = [ov itemAtRow:row];
  if (!item) return;
  uint32_t id_v = (uint32_t)((NSNumber*)item).unsignedIntValue;
  if (!wd->emit_events) return;
  neui_event_t ev = {};
  ev.type             = NEUI_EVENT_TREE_ITEM_ACTIVATED;
  ev.data.tree.widget = { wd->widget_id };
  ev.data.tree.item   = { id_v };
  sess->dispatch_event(&ev);
}

@end

// ---------------------------------------------------------------------------
// NEUINativeTextDelegate.

@interface NEUINativeTextDelegate : NSObject<NSTextFieldDelegate, NSTextViewDelegate>
+ (instancetype)shared;
@end

@implementation NEUINativeTextDelegate

+ (instancetype)shared
{
  static NEUINativeTextDelegate* s = nil;
  static dispatch_once_t once;
  dispatch_once(&once, ^{ s = [[NEUINativeTextDelegate alloc] init]; });
  return s;
}

// NSTextField: edit notifications come via control:textShouldEndEditing: and
// controlTextDidChange:. We use controlTextDidChange: for the live-update path.
- (void)controlTextDidChange:(NSNotification*)notification
{
  NSTextField* f = (NSTextField*)notification.object;
  if (![f isKindOfClass:[NSTextField class]]) return;
  macos_host::dispatch_widget_updated((uint32_t)f.tag);
}

// NSTextView change notification. The widget_id is stashed in the
// TextView's identifier (since NSTextView doesn't have a tag).
- (void)textDidChange:(NSNotification*)notification
{
  NSTextView* tv = (NSTextView*)notification.object;
  if (![tv isKindOfClass:[NSTextView class]]) return;
  NSString* idstr = tv.identifier;
  if (!idstr) return;
  uint32_t widget_id = (uint32_t)[idstr longLongValue];
  macos_host::dispatch_widget_updated(widget_id);
}

@end

// ---------------------------------------------------------------------------
// macos_host::Session::widget_show + Session::run + helpers.

namespace macos_host
{
  // Forward decl - defined below alongside the descendant walker.
  static bool widget_is_native_container(const WidgetData& w);

  // Walk up the widget tree from `idx` until we find an ancestor that
  // can host a child view. Two cases (return the closest one):
  //   - A container widget (SECTION) with an NSView native_control -
  //     return that view, so SECTION children layout in section-local
  //     coords.
  //   - A frame (APPWINDOW / DIALOG / PLUGWINDOW) with native_window -
  //     return its NSWindow.contentView.
  // get_all_parents returns parents in nearest-first order, so the first
  // match wins. Returns nil if no usable ancestor exists yet.
  static NSView* find_parent_content_view(Session* s, uint32_t idx)
  {
    if (!s) return nil;
    auto parents = s->_widgets.get_all_parents(idx);
    for (uint32_t p : parents) {
      if (p == 0) continue;
      if (!s->_widgets.exists(p)) continue;
      auto& pw = s->_widgets[p];
      if (widget_is_native_container(pw) && pw.native_control) {
        // Scrolling section -> inner body view; non-scrolling -> section view.
        NSView* container = section_child_container_macos(pw);
        if (container) return container;
      }
      if (pw.native_window) {
        NSWindow* w = native_window_from(pw.native_window);
        return w.contentView;
      }
    }
    return nil;
  }

  // Released from widgets.mm's Session::widget_destroy.
  void release_native_window_macos(WidgetData& wd)
  {
    if (!wd.native_window) return;
    NSWindow* w = (__bridge_transfer NSWindow*)wd.native_window;
    wd.native_window = nullptr;
    NEUINativeWindowDelegate* d = (NEUINativeWindowDelegate*)w.delegate;
    if (d && d->sheet_active && d->sheet_owner) {
      [d->sheet_owner endSheet:w];
      d->sheet_active = false;
    }
    [w close];
  }

  void release_native_control_macos(WidgetData& wd)
  {
    if (!wd.native_control) return;
    // MENUBAR stores an NSMenu*; everything else stores an NSView subclass.
    // NSMenu is not an NSView - sending removeFromSuperview to it crashes.
    id obj = (__bridge_transfer id)wd.native_control;
    wd.native_control = nullptr;
    if ([obj isKindOfClass:[NEUINativePaintedView class]]) {
      // Painted views own a render context whose per-ctx GPU bitmaps are
      // cached on the session's asset manager (IMAGE source + CUSTOMDRAW
      // compound assets). Drop that cache while the context is still valid -
      // the view's dealloc destroys the context right after. Mirror of the
      // win32 PaintedWndProc WM_DESTROY -> _asset_manager.release_context.
      NEUINativePaintedView* pv = (NEUINativePaintedView*)obj;
      if (wd.session && pv->render_ctx)
        wd.session->_asset_manager.release_context(pv->render_ctx,
                                                   neui_cg_backend::get_backend());
      // Release the scrolling-section inner body view (a subview of pv). The
      // section's tree children have already been torn down bottom-up by the
      // time the section itself is released, so the body view has no live
      // child subviews left. No-op for non-scrolling sections / non-sections.
      section_destroy_body_view_macos(wd);
      [pv removeFromSuperview];
    } else if ([obj isKindOfClass:[NSView class]]) {
      [(NSView*)obj removeFromSuperview];
    } else if ([obj isKindOfClass:[NSMenu class]]) {
      if (NSApp.mainMenu == (NSMenu*)obj) NSApp.mainMenu = nil;
    }
  }

  // Request a repaint of a widget's painted view. Mirror of the win32
  // host's InvalidateRect(hwnd, nullptr, FALSE). Called from
  // Session::invalidate_widgets_with_compound + widget_invalidate +
  // the compound-attached attribute-setter invalidation hooks. No-op
  // for widgets without an NSView backing (native NSControl widgets get
  // their repaint through AppKit's normal invalidation path).
  void mark_widget_dirty_for_paint(WidgetData& wd)
  {
    if (!wd.native_control) return;
    id obj = (__bridge id)wd.native_control;
    if (![obj isKindOfClass:[NSView class]]) return;
    [(NSView*)obj setNeedsDisplay:YES];
  }


  // -------------------------------------------------------------------------
  // Per-type widget_show helpers.

  // Apply pre-show frame-level attribute state: title, icon, min/max size
  // constraints. Same shape as the xpl host's install_view_and_context block.
  static void apply_frame_attrs(NSWindow* window, WidgetData& w)
  {
    if (!w.text.empty())
      [window setTitle:[NSString stringWithUTF8String:w.text.c_str()]];

    if (w.attrs) {
      const char* icon_path = w.attrs->get_string(NEUI_ATTR_ICON_PATH);
      if (icon_path && *icon_path) {
        NSString* ns_path = [NSString stringWithUTF8String:icon_path];
        if (ns_path) {
          NSImage* img = [[NSImage alloc] initWithContentsOfFile:ns_path];
          if (img) [NSApp setApplicationIconImage:img];
        }
      }

      int min_w = w.attrs->get_int(NEUI_ATTR_MIN_WIDTH,  0);
      int min_h = w.attrs->get_int(NEUI_ATTR_MIN_HEIGHT, 0);
      int max_w = w.attrs->get_int(NEUI_ATTR_MAX_WIDTH,  0);
      int max_h = w.attrs->get_int(NEUI_ATTR_MAX_HEIGHT, 0);
      NSSize min_sz = NSMakeSize(min_w > 0 ? min_w : 0,
                                  min_h > 0 ? min_h : 0);
      NSSize max_sz = NSMakeSize(max_w > 0 ? max_w : FLT_MAX,
                                  max_h > 0 ? max_h : FLT_MAX);
      [window setContentMinSize:min_sz];
      [window setContentMaxSize:max_sz];
    }
  }

  static NSWindowStyleMask styles_for_dialog()
  { return neui_detail::styles_for_dialog_macos(); }

  // Generic frame creator. Used for APPWINDOW (full chrome), DIALOG (titled +
  // closable), and PLUGWINDOW (borderless). When `owner` is set + modal, the
  // dialog is presented as a sheet on widget_show; this hook records that
  // intent on the delegate.
  static NSWindow* create_native_frame(Session* s, uint32_t idx, WidgetData& w,
                                        NSWindowStyleMask style_mask,
                                        bool counts_toward_quit,
                                        NSWindow* sheet_owner_or_nil)
  {
    ensure_nsapp_initialised();

    NSRect frame_rect = logical_window_rect(w.x, w.y, w.width, w.height);
    NSWindow* window = [[NSWindow alloc]
       initWithContentRect:frame_rect
                 styleMask:style_mask
                   backing:NSBackingStoreBuffered
                     defer:NO];
    [window setReleasedWhenClosed:NO];

    NEUINativeContentView* cv =
      [[NEUINativeContentView alloc] initWithFrame:NSMakeRect(0, 0, w.width, w.height)];
    [window setContentView:cv];

    // Register the content view as a NSDraggingDestination so AppKit
    // routes external drags into our NSDraggingDestination protocol
    // methods on NEUINativeContentView. The framework gates whether
    // any client event fires via the widget's drop_target flag.
    [cv registerForDraggedTypes:@[
      NSPasteboardTypeString,
      NSPasteboardTypeHTML,
      NSPasteboardTypeFileURL,
    ]];

    // Tab / Shift-Tab focus traversal: the key-view loop is built manually in
    // widget-creation order (see rebuild_key_view_loop_macos, called after the
    // descendants are created) so the order matches the win32 + xpl hosts.
    // AppKit's autorecalculatesKeyViewLoop is left OFF because it orders by
    // geometry (top-to-bottom / left-to-right), which would diverge.
    window.autorecalculatesKeyViewLoop = NO;

    NEUINativeWindowDelegate* d = [[NEUINativeWindowDelegate alloc] init];
    d->session      = s;
    d->widget_index = idx;
    d->is_appwindow = counts_toward_quit;
    d->sheet_owner  = sheet_owner_or_nil;
    [window setDelegate:d];
    objc_setAssociatedObject(window, "NEUINativeWindowDelegate", d,
                              OBJC_ASSOCIATION_RETAIN_NONATOMIC);

    w.native_window = (__bridge_retained void*)window;
    if (counts_toward_quit) ++g_appwindow_count;

    apply_frame_attrs(window, w);
    return window;
  }

  static void create_appwindow(Session* s, uint32_t idx, WidgetData& w)
  {
    NSWindow* window = create_native_frame(s, idx, w,
                                             styles_for_appwindow(),
                                             /*counts_toward_quit*/true,
                                             /*sheet_owner*/nil);
    [window makeKeyAndOrderFront:nil];
  }

  static void create_plugwindow(Session* s, uint32_t idx, WidgetData& w)
  {
    NSWindow* window = create_native_frame(s, idx, w,
                                             NSWindowStyleMaskBorderless,
                                             /*counts_toward_quit*/false,
                                             /*sheet_owner*/nil);
    [window makeKeyAndOrderFront:nil];
  }

  static void create_dialog(Session* s, uint32_t idx, WidgetData& w)
  {
    NSWindow* owner_window = nil;
    if (w.owner_index != 0 && s->_widgets.exists(w.owner_index)) {
      auto& ow = s->_widgets[w.owner_index];
      if (ow.native_window) owner_window = native_window_from(ow.native_window);
    }
    bool modal = !w.attrs || w.attrs->get_int(NEUI_ATTR_MODAL, 1) != 0;

    NSWindow* window = create_native_frame(s, idx, w,
                                             styles_for_dialog(),
                                             /*counts_toward_quit*/false,
                                             modal ? owner_window : nil);
    if (modal && owner_window) {
      NEUINativeWindowDelegate* d =
        objc_getAssociatedObject(window, "NEUINativeWindowDelegate");
      if (d) d->sheet_active = true;
      // The completion handler closes the sheet's window after the
      // close-button path's endSheet: detaches it from the owner. Weak
      // ref so the block doesn't keep the dialog alive past teardown.
      __weak NSWindow* weak_window = window;
      [owner_window beginSheet:window completionHandler:^(NSModalResponse /*r*/){
        NSWindow* w2 = weak_window;
        if (w2 && w2.visible) [w2 close];
      }];
      // Arm the native blocking modal. The actual nested NSEvent pump is
      // run by widget_show AFTER create_descendants_native populates the
      // dialog - pumping here would block before the children are ever
      // created, leaving an empty dialog. windowWillClose: clears the flag
      // to unwind the pump.
      w.modal_pump_active = true;
    } else {
      [window makeKeyAndOrderFront:nil];
    }
  }

  static NSTextField* create_label(WidgetData& w)
  {
    NSTextField* tf = [[NSTextField alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    tf.editable        = NO;
    tf.selectable      = NO;
    tf.bezeled         = NO;
    tf.bordered        = NO;
    tf.drawsBackground = NO;
    tf.stringValue     = [NSString stringWithUTF8String:w.text.c_str() ? w.text.c_str() : ""];
    return tf;
  }

  static NSButton* create_button(WidgetData& w)
  {
    NSButton* b = [[NSButton alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    [b setBezelStyle:NSBezelStyleRounded];
    [b setTitle:[NSString stringWithUTF8String:w.text.c_str() ? w.text.c_str() : ""]];
    b.target = [NEUINativeControlTarget shared];
    b.action = @selector(neuiControlAction:);
    b.tag    = (NSInteger)w.widget_id;
    return b;
  }

  static bool is_readonly(WidgetData& w)
  {
    return w.attrs && w.attrs->get_int(NEUI_ATTR_READONLY, 0) != 0;
  }

  // CSS weight (100..900, 0 = unset) -> AppKit NSFontWeight scale. Mirror of
  // the cg backend's css_weight_to_nsfontweight (kept local so window.mm
  // doesn't depend on the backend's internals).
  static CGFloat css_weight_to_nsfontweight_macos(int weight)
  {
    if (weight <= 0)  return NSFontWeightRegular;
    if (weight < 150) return NSFontWeightUltraLight;
    if (weight < 250) return NSFontWeightThin;
    if (weight < 350) return NSFontWeightLight;
    if (weight < 450) return NSFontWeightRegular;
    if (weight < 550) return NSFontWeightMedium;
    if (weight < 650) return NSFontWeightSemibold;
    if (weight < 750) return NSFontWeightBold;
    if (weight < 850) return NSFontWeightHeavy;
    return NSFontWeightBlack;
  }

  // Apply NEUI_ATTR_FONT_FAMILY / _SIZE / _WEIGHT to a native control's
  // NSFont. Mirror of the win32 host's ensure_custom_font_w32 (WM_SETFONT):
  // when none of the font attrs are set the control keeps its system font;
  // a partial set keeps the unspecified dimensions (size falls back to the
  // control's current point size). An unknown family (e.g. a Windows family
  // like "Consolas") gracefully falls back to the system font at the
  // requested size/weight. NSScrollView-hosted MULTILINE targets its
  // document NSTextView; painted widgets get their font via the cg font
  // stack instead, so this is a no-op for them.
  void apply_font_native_macos(WidgetData& wd)
  {
    if (!wd.native_control || !wd.attrs) return;
    id obj = (__bridge id)wd.native_control;
    NSControl*  ctrl = nil;
    NSTextView* tv   = nil;
    if ([obj isKindOfClass:[NSControl class]]) {
      ctrl = (NSControl*)obj;
    } else if ([obj isKindOfClass:[NSScrollView class]]) {
      NSView* doc = ((NSScrollView*)obj).documentView;
      if ([doc isKindOfClass:[NSTextView class]]) tv = (NSTextView*)doc;
    }
    if (!ctrl && !tv) return;  // tables / painted views: not handled here

    const char* family = wd.attrs->get_string(NEUI_ATTR_FONT_FAMILY);
    float       size   = wd.attrs->get_float (NEUI_ATTR_FONT_SIZE,   0.0f);
    int         weight = wd.attrs->get_int   (NEUI_ATTR_FONT_WEIGHT, 0);
    bool fam_set = family && *family;
    bool sz_set  = size > 0.0f;
    bool wt_set  = weight > 0;
    if (!fam_set && !sz_set && !wt_set) return;  // no override

    NSFont* cur = ctrl ? ctrl.font : tv.font;
    CGFloat eff_size = sz_set ? (CGFloat)size
                              : (cur ? cur.pointSize : [NSFont systemFontSize]);
    CGFloat ns_weight = css_weight_to_nsfontweight_macos(weight);

    NSFont* font = nil;
    if (fam_set) {
      NSString* fam = [NSString stringWithUTF8String:family];
      if (fam) {
        NSFontDescriptor* desc = [NSFontDescriptor fontDescriptorWithFontAttributes:@{
          NSFontFamilyAttribute : fam,
          NSFontTraitsAttribute : @{ NSFontWeightTrait : @(ns_weight) },
        }];
        font = [NSFont fontWithDescriptor:desc size:eff_size];
      }
    }
    if (!font) font = [NSFont systemFontOfSize:eff_size weight:ns_weight];
    if (ctrl) ctrl.font = font;
    else      tv.font   = font;
  }

  // Push WidgetData geometry (x/y/width/height, logical px) into the live
  // native object. Mirror of the win32 host's SetWindowPos path. Child
  // controls get a parent-relative frame (the content view is isFlipped, so
  // (x, y) is top-left); frames get a content-size + screen re-origin
  // (top-left semantics, converted to AppKit's bottom-left screen space);
  // painted views also resize their CG render context. No-op until the
  // native object exists.
  void apply_geometry_native_macos(WidgetData& wd)
  {
    if (wd.native_window) {
      NSWindow* win = (__bridge NSWindow*)wd.native_window;
      [win setContentSize:NSMakeSize(wd.width, wd.height)];
      if (NSScreen.mainScreen) {
        CGFloat sh = NSScreen.mainScreen.frame.size.height;
        [win setFrameTopLeftPoint:NSMakePoint(wd.x, sh - wd.y)];
      }
      return;
    }
    if (!wd.native_control) return;
    id obj = (__bridge id)wd.native_control;
    if (![obj isKindOfClass:[NSView class]]) return;
    NSView* v = (NSView*)obj;
    // If this widget's parent is a scrolling SECTION, subtract its scroll
    // offset so the NSView frame.origin lives where the body-local coords
    // say it does even when the section is scrolled.
    int off_x = 0, off_y = 0;
    if (wd.session)
      parent_scroll_offset_macos(wd.session, wd.index, off_x, off_y);
    [v setFrame:NSMakeRect(wd.x - off_x, wd.y - off_y, wd.width, wd.height)];
    if ([obj isKindOfClass:[NEUINativePaintedView class]]) {
      NEUINativePaintedView* pv = (NEUINativePaintedView*)obj;
      auto* backend = neui_cg_backend::get_backend();
      if (backend && backend->resize && pv->render_ctx)
        backend->resize(pv->render_ctx, (uint32_t)wd.width, (uint32_t)wd.height);
      [pv setNeedsDisplay:YES];
    }
    // Section / page self-resize: rebuild layout + reposition own children.
    if (is_section_like(wd.type))
      section_apply_layout_changes_macos(wd);
    // TABVIEW self-resize: re-flow the chip strip + re-size the selected page
    // to the new content body rect (a setNeedsDisplay drives the paint pass,
    // which calls tabview_apply_page_geometry_macos).
    else if (wd.type && !strcmp(wd.type, NEUI_W_TABVIEW))
      mark_widget_dirty_for_paint(wd);
  }

  // -------------------------------------------------------------------------
  // Scrolling SECTION runtime helpers.
  //
  // The kinetics math + scrollbar geometry live in hosts/shared/
  // widget_section_scroll.h. The macOS host owns the painted view's
  // NSTimer spring-back driver + the NSView frame.origin shuffle when
  // scroll position changes. Mirror of the win32 native helpers in
  // hosts/win32/widgets.cpp (parent_scroll_offset_w32 / ...).

  // Resolve a SECTION's body-fill ARGB: explicit NEUI_ATTR_BACKGROUND wins,
  // else the direction-aware SECTION_BG_LIFT default (lift frame_bg towards
  // white, shade down instead when the lift saturates near-white system bg -
  // see the drawRect: SECTION branch for the rationale). Shared by the
  // section paint + the inner body view's drawRect: so they stay in lockstep.
  uint32_t section_resolve_bg_argb(WidgetData& wd)
  {
    using neui_detail::ColorRole;
    if (wd.attrs && wd.attrs->has(NEUI_ATTR_BACKGROUND))
      return (uint32_t)wd.attrs->get_int(NEUI_ATTR_BACKGROUND, 0);
    uint32_t fbg    = neui_detail::color(ColorRole::frame_bg);
    uint32_t lifted = neui_detail::shade(fbg,  neui_detail::SECTION_BG_LIFT);
    if (lifted == fbg)
      lifted = neui_detail::shade(fbg, -neui_detail::SECTION_BG_LIFT);
    return lifted;
  }

  void section_refresh_scroll_state_macos(WidgetData& wd)
  {
    if (!is_section_like(wd.type)) return;
    const char* mode = wd.attrs ? wd.attrs->get_string(NEUI_ATTR_SCROLL_MODE) : nullptr;
    auto axis = neui_detail::parse_section_scroll_mode(mode);
    if (axis == neui_detail::SectionScrollAxis::None) {
      wd.section_scroll_state.reset();
      return;
    }
    if (!wd.section_scroll_state)
      wd.section_scroll_state =
        std::make_unique<neui_detail::SectionScrollState>();
    wd.section_scroll_state->axis = axis;
  }

  // Create the section's inner body view at the body rect and add it as a
  // subview of the section painted view. Children of a scrolling section
  // parent to this view (see create_descendants_native / find_parent_content_view)
  // so they clip to the body. Retained via __bridge_retained, released by
  // section_destroy_body_view_macos. Mirror of section_create_body_hwnd_w32.
  void section_create_body_view_macos(WidgetData& sec)
  {
    if (!sec.native_control || sec.section_body_view) return;
    id sv = (__bridge id)sec.native_control;
    if (![sv isKindOfClass:[NSView class]]) return;
    section_compute_layout_macos(sec);
    const auto& L = sec.section_last_layout;
    NEUISectionBodyView* body = [[NEUISectionBodyView alloc]
      initWithFrame:NSMakeRect(L.body_x, L.body_y,
                                L.body_w > 0 ? L.body_w : 1,
                                L.body_h > 0 ? L.body_h : 1)];
    body->widget_id = sec.widget_id;
    // NSView does NOT clip its subviews to bounds by default, so a scrolled
    // child would spill over the chip band + out of the section entirely.
    // Layer-backing + masksToBounds gives the clip portably (clipsToBounds is
    // macOS 14+ only). The view stays transparent (no drawRect: fill), so the
    // earlier opaque-fill sibling-rendering regression doesn't recur.
    body.wantsLayer = YES;
    body.layer.masksToBounds = YES;
    [(NSView*)sv addSubview:body];
    sec.section_body_view = (__bridge_retained void*)body;
  }

  void section_destroy_body_view_macos(WidgetData& sec)
  {
    if (!sec.section_body_view) return;
    NSView* body = (__bridge_transfer NSView*)sec.section_body_view;
    sec.section_body_view = nullptr;
    [body removeFromSuperview];
  }

  // Ensure the inner body view exists for any section that needs the band
  // offset (a chip band is present) OR scrolls, then parent the section's
  // children to it. Children's (x, y) is body-relative on every host (the
  // documented contract), so a chip-bearing section needs the body view to
  // shift + clip its children below the band - not just scrolling sections.
  // Created at most once and KEPT for the section's lifetime (see the
  // SCROLL_MODE flip comment). A chip-less, non-scrolling section gets no
  // body view: band_h == 0 makes body-local == section-local, so children
  // parent to the section view directly. No-op if the body view already
  // exists. Mirror of section_ensure_body_hwnd_w32.
  void section_ensure_body_view_macos(WidgetData& sec)
  {
    if (!sec.native_control || sec.section_body_view) return;
    bool scrolling = (sec.section_scroll_state != nullptr);
    int band_h = 0;
    if (!scrolling) {
      band_h = neui_detail::section_band_h_for(section_effective_text_macos(sec),
                                                 sec.height,
                                                 section_effective_align_macos(sec));
    }
    if (!scrolling && band_h <= 0) return;
    section_create_body_view_macos(sec);
    section_reparent_children_macos(sec, /*to_body*/true);
  }

  // Re-parent every direct child NSView of `sec` between the section painted
  // view and the inner body view. Called when SCROLL_MODE flips at runtime.
  // Mirror of section_reparent_children_w32.
  void section_reparent_children_macos(WidgetData& sec, bool to_body)
  {
    if (!sec.native_control || !sec.session) return;
    NSView* new_parent = nil;
    if (to_body) {
      if (!sec.section_body_view) return;
      new_parent = (__bridge NSView*)sec.section_body_view;
    } else {
      id sv = (__bridge id)sec.native_control;
      if (![sv isKindOfClass:[NSView class]]) return;
      new_parent = (NSView*)sv;
    }
    uint32_t idx = sec.session->_widgets.child(sec.index);
    while (idx != 0) {
      if (sec.session->_widgets.exists(idx)) {
        auto& cw = sec.session->_widgets[idx];
        if (cw.native_control) {
          id obj = (__bridge id)cw.native_control;
          if ([obj isKindOfClass:[NSView class]] &&
              ((NSView*)obj).superview != new_parent)
            [new_parent addSubview:(NSView*)obj];
        }
      }
      idx = sec.session->_widgets.next(idx);
    }
  }

  // The NSView that children of `sec` should parent to: the inner body view
  // for a scrolling section, else the section's own painted view. Mirror of
  // section_child_parent_hwnd_w32.
  NSView* section_child_container_macos(const WidgetData& sec)
  {
    if (sec.section_body_view)
      return (__bridge NSView*)sec.section_body_view;
    if (sec.native_control) {
      id obj = (__bridge id)sec.native_control;
      if ([obj isKindOfClass:[NSView class]]) return (NSView*)obj;
    }
    return nil;
  }

  void section_compute_layout_macos(WidgetData& wd)
  {
    if (!wd.session) return;
    int band_h = neui_detail::section_band_h_for(section_effective_text_macos(wd),
                                                  wd.height,
                                                  section_effective_align_macos(wd));
    int initial_body_w = wd.width;
    int initial_body_h = wd.height - band_h;
    if (initial_body_h < 0) initial_body_h = 0;
    int content_w = 0, content_h = 0;
    auto autofn = [&](int& w, int& h){
      neui_detail::section_compute_auto_extent(wd.session->_widgets,
                                                 wd.index, w, h);
    };
    neui_detail::resolve_section_content_extent(wd.attrs.get(), autofn,
                                                  initial_body_w, initial_body_h,
                                                  content_w, content_h);
    auto axis = wd.section_scroll_state
                  ? wd.section_scroll_state->axis
                  : neui_detail::SectionScrollAxis::None;
    wd.section_last_layout = neui_detail::compute_section_layout(
                               wd.width, wd.height, band_h,
                               content_w, content_h, axis);
    if (wd.section_scroll_state) {
      wd.section_scroll_state->content_w = content_w;
      wd.section_scroll_state->content_h = content_h;
      neui_detail::clamp_section_scroll_idle(*wd.section_scroll_state,
                                               content_w, content_h,
                                               wd.section_last_layout.body_w,
                                               wd.section_last_layout.body_h);
    }
  }

  // Reposition every direct child NSView of the SECTION to its scroll-
  // adjusted frame.origin. No-op for non-scrolling sections.
  void section_reposition_children_macos(WidgetData& sec)
  {
    if (!sec.native_control || !sec.session) return;
    int sx = sec.section_scroll_state ? sec.section_scroll_state->scroll_x : 0;
    int sy = sec.section_scroll_state ? sec.section_scroll_state->scroll_y : 0;
    uint32_t idx = sec.session->_widgets.child(sec.index);
    while (idx != 0) {
      if (sec.session->_widgets.exists(idx)) {
        auto& cw = sec.session->_widgets[idx];
        if (cw.native_control) {
          id obj = (__bridge id)cw.native_control;
          if ([obj isKindOfClass:[NSView class]]) {
            NSView* v = (NSView*)obj;
            [v setFrame:NSMakeRect(cw.x - sx, cw.y - sy, cw.width, cw.height)];
          }
        }
      }
      idx = sec.session->_widgets.next(idx);
    }
  }

  void section_apply_layout_changes_macos(WidgetData& sec)
  {
    if (!sec.native_control || !is_section_like(sec.type)) return;
    section_compute_layout_macos(sec);
    // Resize the inner body view to the recomputed body rect before
    // repositioning children (whose frames are body-view-local). The body
    // view's own frame.origin carries the band offset, so the child math in
    // section_reposition_children_macos stays a plain (cw.x - sx, cw.y - sy).
    if (sec.section_body_view) {
      NSView* body = (__bridge NSView*)sec.section_body_view;
      const auto& L = sec.section_last_layout;
      [body setFrame:NSMakeRect(L.body_x, L.body_y,
                                 L.body_w > 0 ? L.body_w : 1,
                                 L.body_h > 0 ? L.body_h : 1)];
      [body setNeedsDisplay:YES];
    }
    section_reposition_children_macos(sec);
    mark_widget_dirty_for_paint(sec);
  }

  // -------------------------------------------------------------------------
  // TABVIEW runtime helpers. The geometry math lives in
  // hosts/shared/widget_tabview.h; here the macOS host enumerates the TABPAGE
  // children, sizes the selected page's NSView to the content body rect,
  // toggles page visibility, and fires the deselect/select events. Mirror of
  // the xpl host's TabViewWidget::collect_pages / apply_page_geometry /
  // select_tab.

  // Collect the TABVIEW's NEUI_W_TABPAGE child indices in creation (tab) order.
  void tabview_collect_pages_macos(WidgetData& tv, std::vector<uint32_t>& out)
  {
    out.clear();
    if (!tv.session) return;
    uint32_t c = tv.session->_widgets.child(tv.index);
    while (c != 0) {
      if (tv.session->_widgets.exists(c)) {
        auto& cw = tv.session->_widgets[c];
        if (cw.type && !strcmp(cw.type, NEUI_W_TABPAGE))
          out.push_back(c);
      }
      c = tv.session->_widgets.next(c);
    }
  }

  // Size the active page to the tabview's content body rect (from the most
  // recent NEUI_ATTR_TAB_POSITION / _STRIP_SIZE) + show it; hide the rest.
  // A page fills the body and its own children are body-relative (chip-less
  // section, body_y == 0). Reads NEUI_ATTR_TAB_POSITION fresh so a geometry
  // re-apply between paints (e.g. a new page added post-show) stays correct.
  void tabview_apply_page_geometry_macos(WidgetData& tv)
  {
    if (!tv.session) return;
    std::vector<uint32_t> pages;
    tabview_collect_pages_macos(tv, pages);
    int count = (int)pages.size();
    if (count == 0) return;
    if (tv.tab_selected < 0)      tv.tab_selected = 0;
    if (tv.tab_selected >= count) tv.tab_selected = count - 1;

    // Use the content rect cached by the last paint (it accounts for the
    // auto vertical strip width, which needs label measurement). Before the
    // first paint it is zero - fall back to a no-strip layout so the page is
    // sized sensibly until the first paint corrects it.
    neui_detail::SectionLayout L = tv.section_last_layout;
    if (L.body_w <= 0 && L.body_h <= 0) {
      const char* pos = tv.attrs ? tv.attrs->get_string(NEUI_ATTR_TAB_POSITION) : nullptr;
      auto tp = neui_detail::parse_tab_position(pos);
      float strip = tv.attrs ? (float)tv.attrs->get_int(NEUI_ATTR_TAB_STRIP_SIZE, 0) : 0.0f;
      neui_detail::TabViewLayout tl =
        neui_detail::compute_tabview_layout((float)tv.width, (float)tv.height, tp.edge, strip);
      L.body_x = (int)tl.body_x; L.body_y = (int)tl.body_y;
      L.body_w = (int)tl.body_w; L.body_h = (int)tl.body_h;
    }

    for (int i = 0; i < count; ++i) {
      auto& pw = tv.session->_widgets[pages[i]];
      bool active = (i == tv.tab_selected);
      pw.x = (int)L.body_x;
      pw.y = (int)L.body_y;
      pw.width  = (int)L.body_w;
      pw.height = (int)L.body_h;
      pw.visible = active;
      if (pw.native_control) {
        id obj = (__bridge id)pw.native_control;
        if ([obj isKindOfClass:[NSView class]]) {
          NSView* v = (NSView*)obj;
          [v setFrame:NSMakeRect(pw.x, pw.y, pw.width, pw.height)];
          [v setHidden:!active];
        }
      }
      // The page's own body view + children re-flow to the new size.
      section_apply_layout_changes_macos(pw);
    }
  }

  // Switch the active tab. If the selection actually changes, fire
  // NEUI_EVENT_TAB_DESELECTED (old) then _SELECTED (new) BEFORE swapping page
  // visibility + repainting, so a client handler can update the incoming
  // page's widgets first. Mirror of xpl TabViewWidget::select_tab.
  void tabview_select_macos(WidgetData& tv, int ni)
  {
    if (!tv.session) return;
    std::vector<uint32_t> pages;
    tabview_collect_pages_macos(tv, pages);
    int count = (int)pages.size();
    if (count == 0) return;
    if (ni < 0)      ni = 0;
    if (ni >= count) ni = count - 1;
    if (ni == tv.tab_selected) return;
    int old = tv.tab_selected;

    if (old >= 0 && old < count) {
      neui_event_t ev{};
      ev.type               = NEUI_EVENT_TAB_DESELECTED;
      ev.data.tab.widget.id = tv.widget_id;
      ev.data.tab.tab_index = (uint32_t)old;
      ev.data.tab.page.id   = tv.session->_widgets[pages[old]].widget_id;
      tv.session->dispatch_event(&ev);
    }
    {
      neui_event_t ev{};
      ev.type               = NEUI_EVENT_TAB_SELECTED;
      ev.data.tab.widget.id = tv.widget_id;
      ev.data.tab.tab_index = (uint32_t)ni;
      ev.data.tab.page.id   = tv.session->_widgets[pages[ni]].widget_id;
      tv.session->dispatch_event(&ev);
    }

    tv.tab_selected = ni;
    tabview_apply_page_geometry_macos(tv);
    mark_widget_dirty_for_paint(tv);
  }

  // Step the SECTION's per-axis spring-back kinetics one frame. Returns
  // true while still animating. Called from sectionBounceTick:.
  bool section_bounce_step_macos(WidgetData& wd)
  {
    if (!wd.section_scroll_state) return false;
    auto& st = *wd.section_scroll_state;
    auto& L  = wd.section_last_layout;
    bool more_v = neui_detail::section_scroll_bounce_step(st, L, false);
    bool more_h = neui_detail::section_scroll_bounce_step(st, L, true);
    section_reposition_children_macos(wd);
    return more_v || more_h;
  }

  // Parent-scroll offset query. If `widget_index`'s parent is a scrolling
  // SECTION, write its scroll offset (logical px) and return; otherwise
  // both outputs stay 0.
  void parent_scroll_offset_macos(Session* sess, uint32_t widget_index,
                                    int& out_x, int& out_y)
  {
    out_x = 0; out_y = 0;
    if (!sess) return;
    uint32_t parent_idx = sess->_widgets.get_parent(widget_index);
    if (parent_idx == 0 || !sess->_widgets.exists(parent_idx)) return;
    auto& pw = sess->_widgets[parent_idx];
    if (pw.section_scroll_state) {
      out_x = pw.section_scroll_state->scroll_x;
      out_y = pw.section_scroll_state->scroll_y;
    }
  }


  // Push WidgetData::enabled into the live native control. Mirror of the
  // win32 host's EnableWindow path (hosts/win32/widgets.cpp::set_enabled +
  // the deferred apply in create_child_windows). Called from w_set_enabled
  // (live change) and from create_native_for_widget (deferred apply right
  // after the NSView/NSControl is instantiated). No-op until the control
  // exists - the flag lives on WidgetData and is re-applied at creation.
  //
  // Three shapes:
  //  - NEUINativePaintedView (IMAGE / KNOB / CUSTOMDRAW / SECTION): no
  //    NSControl setEnabled:; the dim is applied in drawRect: (push_alpha)
  //    and input is gated in the mouse handlers. Just request a repaint.
  //  - NSControl leaves (LABEL / BUTTON / INPUTBOX / CHECKBOX / COMBOBOX /
  //    SLIDER): drive [NSControl setEnabled:] directly - AppKit greys the
  //    control and stops routing input to it.
  //  - NSScrollView-hosted controls (LISTBOX / TREEVIEW = NSTableView /
  //    NSOutlineView, both NSControls; MULTILINE = NSTextView, which is not
  //    an NSControl): reach the document view and disable it there.
  void apply_enabled_native_macos(WidgetData& wd)
  {
    if (!wd.native_control) return;
    id obj = (__bridge id)wd.native_control;
    BOOL en = wd.enabled ? YES : NO;

    if ([obj isKindOfClass:[NEUINativePaintedView class]]) {
      [(NSView*)obj setNeedsDisplay:YES];
      return;
    }
    if ([obj isKindOfClass:[NSControl class]]) {
      [(NSControl*)obj setEnabled:en];
      return;
    }
    if ([obj isKindOfClass:[NSScrollView class]]) {
      NSView* doc = ((NSScrollView*)obj).documentView;
      if ([doc isKindOfClass:[NSControl class]]) {
        [(NSControl*)doc setEnabled:en];
      } else if ([doc isKindOfClass:[NSTextView class]]) {
        // NSTextView is not an NSControl. Re-derive editability from the
        // readonly attr when re-enabling so set_enabled doesn't clobber a
        // client's readonly intent; gate selection + dim the glyphs while
        // disabled. textColor uses the dynamic system colours so it tracks
        // light / dark appearance.
        NSTextView* tv = (NSTextView*)doc;
        tv.editable   = en ? !is_readonly(wd) : NO;
        tv.selectable = en ? YES : NO;
        tv.textColor  = en ? NSColor.textColor : NSColor.disabledControlTextColor;
      }
      return;
    }
  }

  static NEUINativePaintedView* create_painted_view(WidgetData& w)
  {
    NEUINativePaintedView* v = [[NEUINativePaintedView alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    v->widget_id = w.widget_id;
    auto* backend = neui_cg_backend::get_backend();
    if (backend) {
      v->render_ctx = backend->create_context((__bridge void*)v,
                                                (uint32_t)w.width,
                                                (uint32_t)w.height);
    }
    return v;
  }

  static NSSlider* create_slider(WidgetData& w)
  {
    NSSlider* sl = [[NSSlider alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    sl.minValue = 0.0;
    sl.maxValue = 1.0;
    sl.continuous = YES;
    int steps = w.attrs ? w.attrs->get_int(NEUI_ATTR_STEPS, 0) : 0;
    if (steps >= 2) {
      sl.numberOfTickMarks         = steps;
      sl.allowsTickMarkValuesOnly  = YES;
    } else {
      sl.numberOfTickMarks         = 0;
      sl.allowsTickMarkValuesOnly  = NO;
    }
    float v = w.attrs ? w.attrs->get_float(NEUI_PARAM_VALUE, 0.0f) : 0.0f;
    if (v < 0) v = 0; if (v > 1) v = 1;
    sl.doubleValue = v;
    sl.target = [NEUINativeControlTarget shared];
    sl.action = @selector(neuiControlAction:);
    sl.tag    = (NSInteger)w.widget_id;
    return sl;
  }

  static NSScrollView* create_treeview(WidgetData& w)
  {
    NSScrollView* sv = [[NSScrollView alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    sv.hasVerticalScroller = YES;
    sv.borderType          = NSBezelBorder;
    sv.autohidesScrollers  = YES;

    NSOutlineView* ov = [[NSOutlineView alloc]
      initWithFrame:NSMakeRect(0, 0, w.width, w.height)];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"col0"];
    col.width = w.width;
    col.resizingMask = NSTableColumnAutoresizingMask;
    [ov addTableColumn:col];
    ov.outlineTableColumn = col;
    ov.headerView = nil;
    ov.allowsEmptySelection = YES;
    ov.allowsMultipleSelection = NO;

    NEUINativeOutlineSource* src = [[NEUINativeOutlineSource alloc] init];
    src->widget_id = w.widget_id;
    ov.dataSource = src;
    ov.delegate   = src;
    ov.target     = src;
    ov.doubleAction = @selector(neuiOutlineDoubleClick:);

    sv.documentView = ov;
    objc_setAssociatedObject(sv, "NEUINativeOutlineSource", src,
                              OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return sv;
  }

  static NSPopUpButton* create_combobox(WidgetData& w)
  {
    NSPopUpButton* pb = [[NSPopUpButton alloc]
       initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)
           pullsDown:NO];
    pb.target = [NEUINativeControlTarget shared];
    pb.action = @selector(neuiControlAction:);
    pb.tag    = (NSInteger)w.widget_id;

    // Items are typically added via the items API before widget_show, when
    // wd.native_control is still null - the lazy reload in widgets.mm
    // short-circuits in that window. Sync the current items + selection
    // now so the button has its menu populated at first display.
    // -addItemWithTitle: dedupes by title, so use an empty insert then
    // setTitle to preserve duplicates.
    for (auto& it : w.items) {
      [pb addItemWithTitle:@""];
      [pb.lastItem setTitle:[NSString stringWithUTF8String:it.text.c_str()]];
    }
    if (w.selected_item != NEUI_ITEM_NONE
        && w.selected_item < w.items.size())
      [pb selectItemAtIndex:(NSInteger)w.selected_item];

    // The xpl host treats a COMBOBOX's frame as "button + reserved space
    // for the inline drop overlay" - so callers commonly pass an oversized
    // height (e.g. 150). NSPopUpButton is a single-row control whose cell
    // vertically centres its content in the frame, which would float the
    // button down into surrounding widgets. The native dropdown is a real
    // NSMenu and doesn't need the reserved space, so clamp to the cell's
    // intrinsic height, anchored to the frame's top in the flipped view.
    CGFloat intrinsic_h = pb.intrinsicContentSize.height;
    if (intrinsic_h > 0 && intrinsic_h < (CGFloat)w.height)
      pb.frame = NSMakeRect(w.x, w.y, w.width, intrinsic_h);
    return pb;
  }

  static NSScrollView* create_listbox(WidgetData& w)
  {
    NSScrollView* sv = [[NSScrollView alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    sv.hasVerticalScroller = YES;
    sv.borderType          = NSBezelBorder;
    sv.autohidesScrollers  = YES;

    NSTableView* tv = [[NSTableView alloc]
      initWithFrame:NSMakeRect(0, 0, w.width, w.height)];
    NSTableColumn* col = [[NSTableColumn alloc] initWithIdentifier:@"col0"];
    col.width = w.width;
    col.resizingMask = NSTableColumnAutoresizingMask;
    [tv addTableColumn:col];
    tv.headerView         = nil;
    tv.allowsEmptySelection = YES;
    tv.allowsMultipleSelection = NO;

    NEUINativeListSource* src = [[NEUINativeListSource alloc] init];
    src->widget_id = w.widget_id;
    tv.dataSource = src;
    tv.delegate   = src;

    sv.documentView = tv;

    // Keep the source alive for the scroll view's lifetime.
    objc_setAssociatedObject(sv, "NEUINativeListSource", src,
                              OBJC_ASSOCIATION_RETAIN_NONATOMIC);
    return sv;
  }

  static NSButton* create_checkbox(WidgetData& w)
  {
    // SF-Symbol-driven borderless NSButton (not a checkbox cell). On
    // macOS 26 / Sequoia, +checkboxWithTitle: + setState:NSControlStateValueMixed
    // promotes the cell to a bezeled pull-down with up/down chevrons even
    // when allowsMixedState=NO. Driving an SF Symbol image manually
    // avoids the cell promotion path. State is cached on the widget's
    // attrs ("neui.macoshost.checkstate") and the click handler swaps
    // the image.
    NSString* title = [NSString stringWithUTF8String:w.text.c_str() ? w.text.c_str() : ""];
    NSButton* b = [[NSButton alloc] initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    b.title         = title;
    b.bordered      = NO;
    b.buttonType    = NSButtonTypeMomentaryChange;  // disables AppKit's auto-toggle of .state
    b.imagePosition = NSImageLeft;
    b.alignment     = NSTextAlignmentLeft;
    // Initialise from any check state set before widget_show. w_set_check
    // caches the logical state on the attrs but can't touch the NSButton
    // until it exists (deferred creation), so the initial image must be
    // derived from the cached value rather than hardcoded to UNCHECKED.
    int init_state = w.attrs
      ? w.attrs->get_int("neui.macoshost.checkstate", NEUI_CHECK_UNCHECKED)
      : NEUI_CHECK_UNCHECKED;
    b.image         = checkbox_image_for_state(init_state);
    b.target        = [NEUINativeControlTarget shared];
    b.action        = @selector(neuiControlAction:);
    b.tag           = (NSInteger)w.widget_id;
    return b;
  }

  static NSTextField* create_inputbox(WidgetData& w)
  {
    NSTextField* tf = [[NSTextField alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    tf.editable        = !is_readonly(w);
    tf.selectable      = YES;
    tf.bezeled         = YES;
    tf.bordered        = YES;
    tf.drawsBackground = YES;
    tf.stringValue     = [NSString stringWithUTF8String:w.text.c_str() ? w.text.c_str() : ""];
    tf.delegate        = [NEUINativeTextDelegate shared];
    tf.tag             = (NSInteger)w.widget_id;
    return tf;
  }

  // MULTILINE: NSTextView wrapped in NSScrollView so it scrolls + has a frame.
  // The wrapper is what we hand back as native_control (positioned at w.x/y);
  // native_scroll points at the same NSScrollView for ARC bookkeeping. The
  // NSTextView itself is retrieved via [scroll documentView] for stringValue
  // / setString: in widget_get_text / widget_set_text.
  static NSScrollView* create_multiline(WidgetData& w)
  {
    NSScrollView* sv = [[NSScrollView alloc]
      initWithFrame:NSMakeRect(w.x, w.y, w.width, w.height)];
    sv.hasVerticalScroller   = YES;
    sv.hasHorizontalScroller = NO;
    sv.borderType            = NSBezelBorder;
    sv.autohidesScrollers    = YES;

    NSTextView* tv = [[NSTextView alloc]
      initWithFrame:NSMakeRect(0, 0, w.width, w.height)];
    [tv setMinSize:NSMakeSize(0, 0)];
    [tv setMaxSize:NSMakeSize(FLT_MAX, FLT_MAX)];
    tv.verticallyResizable    = YES;
    tv.horizontallyResizable  = NO;
    tv.autoresizingMask       = NSViewWidthSizable;
    tv.editable               = !is_readonly(w);
    tv.selectable             = YES;
    tv.richText               = NO;
    tv.allowsUndo             = YES;
    tv.string                 = [NSString stringWithUTF8String:w.text.c_str() ? w.text.c_str() : ""];
    tv.delegate               = [NEUINativeTextDelegate shared];
    // NSTextView has no tag; stash widget_id on .identifier as a string.
    tv.identifier             = [NSString stringWithFormat:@"%u", w.widget_id];

    sv.documentView = tv;
    return sv;
  }

  // -------------------------------------------------------------------------

  // Per-widget native-control creation. Called from widget_show on a leaf,
  // and recursively for each descendant when a frame is shown for the first
  // time. Other widget types fall through (no-op) until their step lands.
  static void create_native_for_widget(Session* /*s*/, WidgetData& w, NSView* parent_content)
  {
    if (!parent_content || w.native_control || !w.type) return;
    if (!strcmp(w.type, NEUI_W_LABEL)) {
      NSTextField* tf = create_label(w);
      [parent_content addSubview:tf];
      w.native_control = (__bridge_retained void*)tf;
    } else if (!strcmp(w.type, NEUI_W_BUTTON)) {
      NSButton* b = create_button(w);
      [parent_content addSubview:b];
      w.native_control = (__bridge_retained void*)b;
    } else if (!strcmp(w.type, NEUI_W_INPUTBOX)) {
      NSTextField* tf = create_inputbox(w);
      [parent_content addSubview:tf];
      w.native_control = (__bridge_retained void*)tf;
    } else if (!strcmp(w.type, NEUI_W_MULTILINE)) {
      NSScrollView* sv = create_multiline(w);
      [parent_content addSubview:sv];
      w.native_control = (__bridge_retained void*)sv;
    } else if (!strcmp(w.type, NEUI_W_CHECKBOX)
            || !strcmp(w.type, NEUI_W_CHECKBOX3)) {
      NSButton* b = create_checkbox(w);
      [parent_content addSubview:b];
      w.native_control = (__bridge_retained void*)b;
    } else if (!strcmp(w.type, NEUI_W_LISTBOX)) {
      NSScrollView* sv = create_listbox(w);
      [parent_content addSubview:sv];
      w.native_control = (__bridge_retained void*)sv;
    } else if (!strcmp(w.type, NEUI_W_COMBOBOX)) {
      NSPopUpButton* pb = create_combobox(w);
      [parent_content addSubview:pb];
      w.native_control = (__bridge_retained void*)pb;
    } else if (!strcmp(w.type, NEUI_W_TREEVIEW)) {
      NSScrollView* sv = create_treeview(w);
      [parent_content addSubview:sv];
      w.native_control = (__bridge_retained void*)sv;
    } else if (!strcmp(w.type, NEUI_W_SLIDER)) {
      NSSlider* sl = create_slider(w);
      [parent_content addSubview:sl];
      w.native_control = (__bridge_retained void*)sl;
    } else if (!strcmp(w.type, NEUI_W_IMAGE)
            || !strcmp(w.type, NEUI_W_KNOB)
            || !strcmp(w.type, NEUI_W_CUSTOMDRAW)
            || !strcmp(w.type, NEUI_W_GRID)
            || !strcmp(w.type, NEUI_W_TABVIEW)
            || is_section_like(w.type)) {
      // SECTION / TABPAGE use a painted view for the body fill + title chip;
      // child widgets nest into it via the recursive descendant walker (see
      // create_descendants_native). For scrolling sections/pages
      // (NEUI_ATTR_SCROLL_MODE != "none") the painted view also intercepts
      // wheel / mouse for scrollbar drag + kinetics; non-scrolling stay
      // pointer-pass-through. TABVIEW is a painted container that draws the
      // chip strip + shows the selected page.
      NEUINativePaintedView* v = create_painted_view(w);
      [parent_content addSubview:v];
      w.native_control = (__bridge_retained void*)v;
      if (is_section_like(w.type)) {
        // A SECTION / TABPAGE always clips its children to its bounds,
        // matching the win32 host (HWND parenting clips child windows
        // inherently). NSView does NOT clip subviews by default, so without
        // this a non-scrolling section - or one just switched to
        // scroll_mode="none" - lets overflowing children spill outside the
        // section rect. Scrolling sections additionally clip to the body via
        // the inner body view.
        v.wantsLayer = YES;
        v.layer.masksToBounds = YES;
        section_refresh_scroll_state_macos(w);
        // Sections with a chip band (or that scroll) get an inner body view
        // so children sit + clip below the band, matching the body-relative
        // child-coordinate contract. Created here, before
        // create_descendants_native descends into the children and parents
        // them to the body via section_child_container_macos. (A TABPAGE is
        // chip-less, so it only gets the body view when it scrolls.)
        section_ensure_body_view_macos(w);
      } else if (!strcmp(w.type, NEUI_W_TABVIEW)) {
        // TABVIEW clips its (TABPAGE) children to its bounds; the selected
        // page is positioned to the content body rect each paint.
        v.wantsLayer = YES;
        v.layer.masksToBounds = YES;
      }
    }

    // Apply any pre-show enabled state now that the native control exists.
    // Children default to enabled=true, so this only matters when the client
    // disabled the widget before widget_show. Mirror of the win32 host's
    // deferred EnableWindow in create_child_windows.
    if (w.native_control && !w.enabled)
      apply_enabled_native_macos(w);

    // Apply any pre-show custom font (NEUI_ATTR_FONT_*) - no-op when unset.
    if (w.native_control)
      apply_font_native_macos(w);
  }

  // True for widget types whose NSView should act as the parent container
  // for nested children. Today only SECTION needs this - the rest are
  // leaves (LABEL / BUTTON / INPUTBOX / ...) or are addressed through
  // dedicated paths (MENUBAR is an NSMenu, not an NSView). CUSTOMDRAW is
  // intentionally NOT a container - matching the win32 native host's
  // behaviour where child HWNDs of a CUSTOMDRAW HWND paint above the
  // parent's compound layer stack but don't get z-interleaved with it.
  static bool widget_is_native_container(const WidgetData& w)
  {
    // SECTION + TABPAGE (a chip-less section) nest their content children;
    // TABVIEW nests its TABPAGE children. For TABVIEW / a chip-less,
    // non-scrolling TABPAGE there is no inner body view, so
    // section_child_container_macos falls back to the painted view itself.
    return is_section_like(w.type) ||
           (w.type && !strcmp(w.type, NEUI_W_TABVIEW));
  }

  // After a frame is created, walk every descendant and instantiate its
  // native control. Mirror of hosts/win32/widgets.cpp::create_child_windows.
  // MENUBAR children are special: the NSMenu was already allocated at
  // widget_create time, and instead of adding a subview we hand it to
  // [NSApp setMainMenu:] so the system menu bar shows the items.
  //
  // For container widgets (SECTION today) the recursion descends with the
  // container's NSView as the new parent, so children with their
  // section-local (x, y) lay out correctly inside it. For non-containers
  // descendants keep parenting to the same enclosing view - mirrors the
  // pre-section behaviour and avoids leaves like NSButton accidentally
  // becoming hosts for unrelated child widgets.
  // The NSView that represents this widget in the key-view loop, or nil if
  // it isn't a tab stop. The tab-stop SET mirrors the win32 host's WS_TABSTOP
  // controls (BUTTON / INPUTBOX / MULTILINE / CHECKBOX[3] / LISTBOX / COMBOBOX
  // / TREEVIEW / SLIDER / CUSTOMDRAW); LABEL / SECTION / IMAGE / KNOB are not.
  // NEUI_ATTR_TAB_STOP = 0 removes a widget explicitly (default on). For
  // NSScrollView-hosted controls the document view (table / outline / text)
  // is the responder, so it - not the scroll container - goes in the loop.
  static NSView* tab_stop_view_macos(WidgetData& w)
  {
    if (!w.native_control || !w.type || !w.visible) return nil;
    if (w.attrs && w.attrs->has(NEUI_ATTR_TAB_STOP) &&
        w.attrs->get_int(NEUI_ATTR_TAB_STOP, 1) == 0)
      return nil;
    const char* t = w.type;
    bool is_stop =
      !strcmp(t, NEUI_W_BUTTON)   || !strcmp(t, NEUI_W_INPUTBOX)  ||
      !strcmp(t, NEUI_W_MULTILINE)|| !strcmp(t, NEUI_W_CHECKBOX)  ||
      !strcmp(t, NEUI_W_CHECKBOX3)|| !strcmp(t, NEUI_W_LISTBOX)   ||
      !strcmp(t, NEUI_W_COMBOBOX) || !strcmp(t, NEUI_W_TREEVIEW)  ||
      !strcmp(t, NEUI_W_SLIDER)   || !strcmp(t, NEUI_W_CUSTOMDRAW)||
      !strcmp(t, NEUI_W_GRID);
    if (!is_stop) return nil;
    id obj = (__bridge id)w.native_control;
    if (![obj isKindOfClass:[NSView class]]) return nil;
    NSView* v = (NSView*)obj;
    if ([v isKindOfClass:[NSScrollView class]]) {
      NSView* doc = ((NSScrollView*)v).documentView;
      if (doc) return doc;
    }
    return v;
  }

  // Collect tab-stop views under `parent_idx` in widget-creation order
  // (pre-order DFS: node before its descendants, siblings in insertion order)
  // - identical to the win32 z-order walk + xpl collect_tab_stops.
  static void collect_tab_stops_macos(Session* s, uint32_t parent_idx,
                                        std::vector<NSView*>& out)
  {
    uint32_t i = s->_widgets.child(parent_idx);
    while (i != 0) {
      if (s->_widgets.exists(i)) {
        NSView* v = tab_stop_view_macos(s->_widgets[i]);
        if (v) out.push_back(v);
        collect_tab_stops_macos(s, i, out);
      }
      i = s->_widgets.next(i);
    }
  }

  // Wire the frame's key-view loop in creation order so Tab / Shift-Tab match
  // win32 + xpl. Disabled controls left in the chain are skipped automatically
  // by AppKit (they return NO from acceptsFirstResponder), so the loop only
  // needs rebuilding when widgets are added / removed - done on each show.
  void rebuild_key_view_loop_macos(Session* s, uint32_t frame_idx, NSWindow* window)
  {
    if (!window) return;
    std::vector<NSView*> stops;
    collect_tab_stops_macos(s, frame_idx, stops);
    window.autorecalculatesKeyViewLoop = NO;
    if (stops.empty()) return;
    const size_t n = stops.size();
    for (size_t k = 0; k < n; ++k)
      [stops[k] setNextKeyView:stops[(k + 1) % n]];
    if (!window.initialFirstResponder)
      window.initialFirstResponder = stops.front();
  }

  static void create_descendants_native(Session* s, uint32_t parent_idx,
                                         NSView* parent_content)
  {
    uint32_t i = s->_widgets.child(parent_idx);
    while (i != 0) {
      if (s->_widgets.exists(i)) {
        auto& cw = s->_widgets[i];
        if (cw.type && !strcmp(cw.type, NEUI_W_MENUBAR)) {
          if (cw.native_control) {
            NSMenu* m = (__bridge NSMenu*)cw.native_control;
            [NSApp setMainMenu:m];
          }
        } else {
          create_native_for_widget(s, cw, parent_content);
          // Honour a pre-show hide(): the fresh NSView is visible by
          // default, so apply the stored flag (matches the xpl host,
          // which no longer re-shows descendants on frame show).
          if (!cw.visible && cw.native_control) {
            id obj = (__bridge id)cw.native_control;
            if ([obj isKindOfClass:[NSView class]])
              [(NSView*)obj setHidden:YES];
          }
        }
        NSView* child_parent = parent_content;
        if (widget_is_native_container(cw) && cw.native_control) {
          // For a scrolling SECTION this is the inner body view; otherwise
          // the section's own painted view (section_child_container_macos
          // resolves both cases).
          NSView* container = section_child_container_macos(cw);
          if (container) child_parent = container;
        }
        create_descendants_native(s, i, child_parent);
      }
      i = s->_widgets.next(i);
    }
  }

  // Immediate realization for a widget created AFTER its containing frame
  // was already shown. Mirror of the win32 host's "parent HWND already
  // exists" branch in widget_create: before widget_show there is no
  // realized ancestor, so find_parent_content_view returns nil and we
  // defer to create_descendants_native at show time; once the frame is up,
  // a freshly-created child needs its NSView built right away (otherwise it
  // never appears - e.g. the section example's Regenerate button rebuilding
  // rows). Called from Session::widget_create in widgets.mm.
  void realize_widget_macos(Session* s, uint32_t idx)
  {
    if (!s || !s->_widgets.exists(idx)) return;
    auto& w = s->_widgets[idx];
    if (w.native_control || w.native_window) return;  // already realized
    if (w.type && !strcmp(w.type, NEUI_W_MENUBAR)) return;  // NSMenu path
    // No realized ancestor frame/container yet -> defer to widget_show.
    NSView* parent_content = find_parent_content_view(s, idx);
    if (!parent_content) return;
    create_native_for_widget(s, w, parent_content);
    if (!w.native_control) return;
    // If the parent is a scrolling SECTION, re-layout it: content extent
    // grew, the body view + scrollbar geometry need refresh, and the new
    // child must be repositioned at scroll-adjusted body-local coords
    // (create_native_for_widget placed it at raw (x,y)).
    uint32_t pidx = s->_widgets.get_parent(idx);
    if (pidx && s->_widgets.exists(pidx)) {
      auto& pw = s->_widgets[pidx];
      if (is_section_like(pw.type) && pw.section_scroll_state)
        section_apply_layout_changes_macos(pw);
      // A TABPAGE added to an already-shown TABVIEW needs the tabview's chip
      // strip re-flowed + its page geometry re-applied (hide the new page if
      // it isn't selected). Repaint drives tabview_apply_page_geometry_macos.
      else if (pw.type && !strcmp(pw.type, NEUI_W_TABVIEW)) {
        tabview_apply_page_geometry_macos(pw);
        mark_widget_dirty_for_paint(pw);
      }
    }
    // Join the frame's Tab / Shift-Tab key-view loop if the new widget is a
    // tab stop. Gate on tab_stop_view_macos so non-focusable additions
    // (LABEL / SECTION / IMAGE) don't trigger a whole-tree loop rebuild.
    if (tab_stop_view_macos(w)) {
      auto parents = s->_widgets.get_all_parents(idx);
      for (uint32_t p : parents) {
        if (p && s->_widgets.exists(p) && s->_widgets[p].native_window) {
          rebuild_key_view_loop_macos(s, p,
            native_window_from(s->_widgets[p].native_window));
          break;
        }
      }
    }
  }

  void Session::widget_show(neui_widget_t widget)
  {
    uint32_t index = widget.id & 0xffff;
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    w.visible = true;

    if (w.native_window) {
      [native_window_from(w.native_window) makeKeyAndOrderFront:nil];
      return;
    }
    if (w.native_control) {
      [native_view_from(w.native_control) setHidden:NO];
      return;
    }

    if (w.isroot) {
      if (!w.type) return;
      if (!strcmp(w.type, NEUI_W_APPWINDOW)) {
        create_appwindow(this, index, w);
      } else if (!strcmp(w.type, NEUI_W_PLUGWINDOW)) {
        create_plugwindow(this, index, w);
      } else if (!strcmp(w.type, NEUI_W_DIALOG)) {
        create_dialog(this, index, w);
      } else {
        return;
      }
      // Now that contentView exists, recursively create native controls for
      // every descendant. This is the equivalent of the win32 host's
      // WM_CREATE -> create_child_windows path.
      NSView* cv = native_window_from(w.native_window).contentView;
      create_descendants_native(this, index, cv);
      // Build the Tab / Shift-Tab key-view loop in creation order now that all
      // descendant controls exist.
      rebuild_key_view_loop_macos(this, index, native_window_from(w.native_window));
      // A modal DIALOG blocks here - AFTER its children exist - spinning a
      // nested NSEvent pump until windowWillClose: clears modal_pump_active
      // (create_dialog armed the flag + presented the sheet). Do not touch
      // `w` after this returns: the dialog is typically destroyed (slot
      // freed) by the OK / close handler that unwound the pump.
      if (w.modal_pump_active)
        neui_detail::run_modal_pump_macos(&w.modal_pump_active);
      return;
    }

    // Leaf widget shown directly (parent frame already exists).
    NSView* parent_content = find_parent_content_view(this, index);
    create_native_for_widget(this, w, parent_content);
  }

  bool Session::run()
  {
    ensure_nsapp_initialised();
    if (g_appwindow_count == 0) return true;
    [NSApp run];
    return true;
  }

} // namespace macos_host
