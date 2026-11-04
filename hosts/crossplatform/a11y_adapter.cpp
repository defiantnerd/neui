#include "a11y_adapter.h"

#include <cstring>
#include <memory>

namespace xpl_host
{
  using neui_detail::A11yInput;
  using neui_detail::A11yNode;
  using neui_detail::A11yNodeId;
  using neui_detail::A11ySubKind;

  namespace
  {
    // Cell sub_index packing: row * kCellRowStride + col.
    //
    // The stride is FIXED rather than the live column count on purpose. With a
    // live count, adding or removing a column would silently re-point every
    // existing cell id at a different cell - an AT holding a reference across a
    // column change would then read out the wrong data with no way to notice.
    // A constant stride caps the grid at 1024 columns and ~2M rows for
    // accessibility purposes, both far past the point where a table is usable.
    constexpr int32_t kCellRowStride = 1024;

    // An enclosing clip in frame-local logical px. Absent = not clipped. Fed to
    // A11yInput::clip_*, which is what makes a scrolled-away node report
    // OFFSCREEN instead of a bogus position (and what makes a SECTION derive as
    // a scroll area rather than a plain group).
    struct Clip
    {
      bool has = false;
      int  x = 0, y = 0, w = 0, h = 0;
    };

    // Innermost wins, but an outer clip still applies - a row inside a list
    // inside a scrolled section is bounded by both.
    Clip clip_intersect(const Clip& outer, const Clip& inner)
    {
      if (!outer.has) return inner;
      if (!inner.has) return outer;
      Clip r;
      r.has = true;
      const int x0 = outer.x > inner.x ? outer.x : inner.x;
      const int y0 = outer.y > inner.y ? outer.y : inner.y;
      const int x1 = (outer.x + outer.w) < (inner.x + inner.w)
                       ? (outer.x + outer.w) : (inner.x + inner.w);
      const int y1 = (outer.y + outer.h) < (inner.y + inner.h)
                       ? (outer.y + outer.h) : (inner.y + inner.h);
      r.x = x0; r.y = y0;
      r.w = x1 > x0 ? (x1 - x0) : 0;
      r.h = y1 > y0 ? (y1 - y0) : 0;
      return r;
    }

    void apply_clip(A11yInput& row, const Clip& c)
    {
      if (!c.has) return;
      row.has_clip = true;
      row.clip_x = c.x; row.clip_y = c.y;
      row.clip_w = c.w; row.clip_h = c.h;
    }

    bool type_is(const WidgetData& wd, const char* want)
    {
      return wd.type && want && !std::strcmp(wd.type, want);
    }

    // ---- Node identity ------------------------------------------------------

    A11yNodeId make_id(Session& s, uint32_t slot, A11ySubKind kind, int32_t sub)
    {
      A11yNodeId id;
      id.widget_id  = s._widgets.exists(slot) ? s._widgets[slot].widget_id : 0;
      id.generation = s.a11y_generation(slot);
      id.sub_kind   = static_cast<int32_t>(kind);
      id.sub_index  = sub;
      return id;
    }

    // ---- Client declarations ------------------------------------------------

    // Copy the neui.a11y.* keys off the widget's bag onto the row. Note this
    // reads the SAME storage NEUI_API_A11Y writes, so a client that bypasses the
    // typed setters and writes the attrs directly is equally visible here - see
    // docs/deferred-issues.md on the component-JSON gap. That also means the
    // typed setters' validation is not a guarantee at this point, which is why
    // the shared model re-checks its own inputs.
    void read_declarations(const neui_detail::AttrBag* bag, A11yInput& row)
    {
      if (!bag) return;
      row.declared_role = bag->get_int(NEUI_ATTR_A11Y_ROLE, NEUI_A11Y_ROLE_DEFAULT);
      row.name          = bag->get_string(NEUI_ATTR_A11Y_NAME);
      row.description   = bag->get_string(NEUI_ATTR_A11Y_DESCRIPTION);
      row.state_mask    = static_cast<uint32_t>(bag->get_int(NEUI_ATTR_A11Y_STATE_MASK, 0));
      row.state_values  = static_cast<uint32_t>(bag->get_int(NEUI_ATTR_A11Y_STATE_VALUES, 0));

      // An explicit a11y value text wins; NEUI_ATTR_VALUE_TEXT is the fallback,
      // so a KNOB that already draws a readout is announced with that readout
      // for free rather than as a bare percentage.
      const char* vt = bag->get_string(NEUI_ATTR_A11Y_VALUE_TEXT);
      if (!vt || !*vt) vt = bag->get_string(NEUI_ATTR_VALUE_TEXT);
      row.value_text = vt;

      // A range needs both ends; a half-declared range would map onto a default
      // the client never asked for.
      if (bag->has(NEUI_ATTR_A11Y_RANGE_MIN) && bag->has(NEUI_ATTR_A11Y_RANGE_MAX)) {
        row.has_range = true;
        row.vmin  = bag->get_float(NEUI_ATTR_A11Y_RANGE_MIN, 0.0f);
        row.vmax  = bag->get_float(NEUI_ATTR_A11Y_RANGE_MAX, 1.0f);
        row.vstep = bag->get_float(NEUI_ATTR_A11Y_RANGE_STEP, 0.0f);
      }
      // A declared a11y value overrides the widget's own - the point of set_value
      // is a CUSTOMDRAW whose value the framework cannot see.
      if (bag->has(NEUI_ATTR_A11Y_VALUE)) {
        row.has_value = true;
        row.value     = bag->get_float(NEUI_ATTR_A11Y_VALUE, 0.0f);
      }
    }

    // labelled_by is resolved separately: it names a WIDGET, so it has to be
    // turned into that widget's current-generation node id.
    void read_labelled_by(Session& s, const neui_detail::AttrBag* bag,
                          A11yInput& row)
    {
      if (!bag || !bag->has(NEUI_ATTR_A11Y_LABELLED_BY)) return;
      const uint32_t wid =
        static_cast<uint32_t>(bag->get_int(NEUI_ATTR_A11Y_LABELLED_BY, 0));
      if (wid == 0 || wid == widget_none.id) return;
      // Same session only: the label has to be a node in THIS tree.
      if (((wid >> 16) & 0xffff) != (s.get_session_id() & 0xffff)) return;
      const uint32_t slot = wid & 0xffff;
      if (!s._widgets.exists(slot)) return;
      row.labelled_by = make_id(s, slot, A11ySubKind::widget, -1);
    }

    // ---- Per-widget framework state -----------------------------------------

    struct WalkCtx
    {
      Session&                s;
      std::vector<A11yInput>& out;
      bool                    modal_blocked = false;
    };

    // Fill the framework-derived half of a widget row. Geometry comes from the
    // cached frame-local abs_x/abs_y that Session::ensure_abs_positions just
    // refreshed - the SAME numbers hit-testing uses, so what an AT points at and
    // what a click lands on cannot disagree.
    A11yInput widget_row(WalkCtx& c, uint32_t slot, const A11yNodeId& parent,
                         const Clip& clip)
    {
      Session& s   = c.s;
      WidgetData& wd = s._widgets[slot];
      const auto* bag = wd.attrs.get();

      A11yInput row;
      row.id     = make_id(s, slot, A11ySubKind::widget, -1);
      row.parent = parent;
      row.type   = wd.type;
      row.x = wd.abs_x; row.y = wd.abs_y;
      row.w = wd.width; row.h = wd.height;
      apply_clip(row, clip);

      row.visible       = wd.visible;
      row.enabled       = wd.enabled;
      row.focused       = (slot == s._focused_widget);
      row.tab_stop      = wd.tab_stop;
      row.modal_blocked = c.modal_blocked;
      row.text          = wd.text.empty() ? nullptr : wd.text.c_str();

      if (bag) {
        row.readonly = bag->get_int(NEUI_ATTR_READONLY, 0) != 0;
        row.password = bag->get_int(NEUI_ATTR_PASSWORD, 0) != 0;
      }
      row.multiline = type_is(wd, NEUI_W_MULTILINE);

      if (auto* cb = dynamic_cast<CheckboxWidget*>(&wd))
        row.check_state = cb->check_state;

      // Does this widget scroll its OWN content? Read from the scroll state the
      // widget actually allocated (NEUI_ATTR_SCROLL_MODE != "none"), not from
      // whether it happens to sit inside something clipped.
      row.scrollable = (wd.scroll_state_ptr() != nullptr);

      // A COMBOBOX is the one widget whose own expansion is a framework fact.
      if (type_is(wd, NEUI_W_COMBOBOX)) {
        row.expandable = true;
        row.expanded   = (s._open_combo == slot);
      }

      // KNOB / SLIDER carry a normalized value the framework owns. Read it
      // before the declarations so an explicit a11y value can override it.
      if (type_is(wd, NEUI_W_KNOB) || type_is(wd, NEUI_W_SLIDER)) {
        row.has_value = true;
        row.value     = bag ? bag->get_float(NEUI_PARAM_VALUE, 0.0f) : 0.0f;
      }

      read_declarations(bag, row);
      read_labelled_by(s, bag, row);

      // A TEXT FIELD's contents are its VALUE, not its name. Without this the
      // text only ever reached the name fallback, so a field with a name (or a
      // labelled_by - the very idiom set_labelled_by exists for) announced the
      // label and offered no value at all: an AT could not read what the user
      // had typed. A password field is deliberately excluded here as well as in
      // the provider, so the contents are not sitting in the tree at all.
      if ((type_is(wd, NEUI_W_INPUTBOX) || type_is(wd, NEUI_W_MULTILINE)) &&
          !row.password && (!row.value_text || !*row.value_text))
        row.value_text = row.text;

      return row;
    }

    // ---- Sub-elements -------------------------------------------------------
    //
    // Each emitter below mirrors one widget's paint geometry. They are the part
    // most at risk of drifting from what is drawn, which is why every one of
    // them reads the same layout helper the paint code reads (grid_compute_
    // viewport, ComboBoxWidget::overlay_rect, TabViewWidget::chips) rather than
    // recomputing the arithmetic.

    // Every emitter below takes the container's INDEX in `c.out`, never a
    // reference: they push rows themselves, and a push can reallocate the vector
    // out from under a reference held across it. The index stays valid.

    // LISTBOX / COMBOBOX rows. The collapsed COMBOBOX bar shows one row, so its
    // rows only exist as an on-screen thing while the overlay is open - and then
    // they are positioned by the overlay, not by the widget rect.
    void emit_list_rows(WalkCtx& c, uint32_t slot, const A11yNodeId& parent,
                        const Clip& clip, size_t ci)
    {
      Session& s = c.s;
      auto* lw = dynamic_cast<ListItemsWidget*>(&s._widgets[slot]);
      if (!lw) return;
      const int n = static_cast<int>(lw->items.size());
      if (n == 0) return;

      auto* combo = dynamic_cast<ComboBoxWidget*>(lw);
      const bool is_open_combo = combo && s._open_combo == slot;
      if (combo && !is_open_combo) {
        // Closed: the drop rows are not on screen. The selected item's text is
        // already the widget's own text, so nothing is lost by omitting them,
        // and emitting invisible rows would have the AT read the whole list on
        // every pass over the control.
        c.out[ci].total_child_count = n;
        c.out[ci].first_child_index = 0;
        return;
      }

      int row_x = lw->abs_x, row_y = lw->abs_y, row_w = lw->width;
      int clip_h = lw->height;
      int visible = 0;
      const int row_h = list_row_height();
      if (row_h <= 0) return;
      // The overlay is drawn OUTSIDE the widget walk, so no ancestor clip
      // applies to its rows; an in-place list's rows inherit the walk's clip
      // normally. Getting this wrong marks visible, clickable rows OFFSCREEN,
      // and a11y_hit_test skips those - the AT cannot reach them.
      bool inherit_ancestor_clip = true;

      if (is_open_combo) {
        // The overlay rect is the single source of truth shared with paint +
        // hit-test, including the flip above the bar when the list would
        // overflow. Needs the backend for its auto-fit width measurement.
        if (!s._backend) return;
        auto r = combo->overlay_rect(s._backend);
        row_x  = static_cast<int>(r.x);
        row_y  = static_cast<int>(r.y);
        row_w  = static_cast<int>(r.w);
        clip_h = static_cast<int>(r.h);
        visible = combo->max_drop_visible();
        // Session::paint_frame paints the overlay AFTER paint_widgets_recursive,
        // at frame identity with every clip popped, and the click handlers
        // hit-test it at frame level too. So a drop list that extends past an
        // enclosing scrolling SECTION is genuinely on screen and genuinely
        // clickable, and must not be reported as clipped by that section.
        inherit_ancestor_clip = false;
      } else {
        // Ceiling division, matching paint_scrollable_list: a partial trailing
        // row IS drawn and IS clickable (ListItemsWidget::on_mouse_event divides
        // the raw offset by the row height), so omitting it would hide a real
        // row from the AT. The clip below keeps it partially-inside rather than
        // OFFSCREEN.
        visible = (lw->height + row_h - 1) / row_h;
        // The scrollbar steals a gutter from the row width once it shows.
        if (n > lw->height / row_h) row_w -= scrollbar_gutter_width();
      }
      if (visible < 1) visible = 1;
      if (row_w < 0) row_w = 0;

      const int first = static_cast<int>(lw->scroll_offset);
      const Clip own_clip{ true, row_x, row_y, row_w, clip_h };
      Clip rows_clip = inherit_ancestor_clip
                         ? clip_intersect(clip, own_clip) : own_clip;

      c.out[ci].total_child_count = n;
      c.out[ci].first_child_index = first;

      for (int i = 0; i < visible; ++i) {
        const int item = first + i;
        if (item < 0 || item >= n) break;
        A11yInput r;
        r.id     = make_id(s, slot, A11ySubKind::list_row, item);
        r.parent = parent;
        r.x = row_x; r.y = row_y + i * row_h; r.w = row_w; r.h = row_h;
        apply_clip(r, rows_clip);
        r.visible       = true;
        r.enabled       = lw->enabled;
        r.modal_blocked = c.modal_blocked;
        r.selected      = (static_cast<uint32_t>(item) == lw->selected_item);
        const std::string& t = lw->items[static_cast<size_t>(item)].text;
        r.text = t.empty() ? nullptr : t.c_str();
        c.out.push_back(r);
      }
    }

    // TREEVIEW items, preserving nesting: each row's parent is the nearest
    // ancestor item that is also a visible row, so an AT gets the real hierarchy
    // rather than a flat list of indented strings.
    void emit_tree_items(WalkCtx& c, uint32_t slot, const A11yNodeId& parent,
                         const Clip& clip, size_t ci)
    {
      Session& s = c.s;
      auto* tv = dynamic_cast<TreeviewWidget*>(&s._widgets[slot]);
      if (!tv) return;
      auto rows = tv->flatten_visible();
      const int n = static_cast<int>(rows.size());
      c.out[ci].total_child_count = n;
      c.out[ci].first_child_index = static_cast<int>(tv->scroll_offset);
      if (n == 0) return;

      const int row_h = tree_row_height();
      if (row_h <= 0) return;
      const int first = static_cast<int>(tv->scroll_offset);
      // Ceiling division, matching TreeviewWidget::paint - see emit_list_rows.
      int visible = (tv->height + row_h - 1) / row_h;
      if (visible < 1) visible = 1;
      int row_w = tv->width;
      if (n > tv->height / row_h) row_w -= scrollbar_gutter_width();
      if (row_w < 0) row_w = 0;

      const Clip rows_clip = clip_intersect(
        clip, Clip{ true, tv->abs_x, tv->abs_y, row_w, tv->height });

      // Which items are INSIDE the emitted window. A row's parent link may only
      // name one of these: naming an item that was windowed out gives an id no
      // node carries, and build_a11y_tree turns a row whose parent it cannot
      // resolve into a ROOT of the frame - i.e. a tree item announced as a
      // sibling of the window, outside the treeview entirely.
      std::vector<uint32_t> in_window;
      in_window.reserve(static_cast<size_t>(visible));
      for (int i = 0; i < visible; ++i) {
        const int vis_row = first + i;
        if (vis_row < 0 || vis_row >= n) break;
        in_window.push_back(rows[static_cast<size_t>(vis_row)].id);
      }
      auto windowed = [&in_window](uint32_t id) {
        for (uint32_t x : in_window) if (x == id) return true;
        return false;
      };

      for (int i = 0; i < visible; ++i) {
        const int vis_row = first + i;
        if (vis_row < 0 || vis_row >= n) break;
        const auto& vr = rows[static_cast<size_t>(vis_row)];
        auto it = tv->tree_items.find(vr.id);
        if (it == tv->tree_items.end()) continue;

        A11yInput r;
        // sub_index is the ITEM id, not the row position: a row position shifts
        // whenever an ancestor is collapsed, so an id built from it would name a
        // different item after any expand / collapse.
        r.id = make_id(s, slot, A11ySubKind::tree_item,
                       static_cast<int32_t>(vr.id));
        // Real hierarchy, read from the item model rather than reconstructed
        // from emission order (which windowing breaks: scroll down and the
        // parents of the first visible rows are no longer in the walk). Climb to
        // the nearest ancestor that IS in the window; if none is, the treeview
        // itself is the honest parent - that ancestor is not in this tree.
        A11yNodeId pid = parent;
        uint32_t up = it->second.parent_id;
        for (size_t guard = 0; guard <= in_window.size() && up != 0; ++guard) {
          if (windowed(up)) {
            pid = make_id(s, slot, A11ySubKind::tree_item,
                          static_cast<int32_t>(up));
            break;
          }
          auto ancestor = tv->tree_items.find(up);
          if (ancestor == tv->tree_items.end()) break;
          up = ancestor->second.parent_id;
        }
        r.parent = pid;
        r.x = tv->abs_x; r.y = tv->abs_y + i * row_h;
        r.w = row_w; r.h = row_h;
        apply_clip(r, rows_clip);
        r.visible       = true;
        r.enabled       = tv->enabled && it->second.enabled;
        r.modal_blocked = c.modal_blocked;
        r.selected      = (vr.id == tv->selected_tree_item);
        r.expandable    = vr.has_children;
        r.expanded      = it->second.expanded;
        r.text = it->second.text.empty() ? nullptr : it->second.text.c_str();
        c.out.push_back(r);
      }
    }

    // GRID: column headers, then the VISIBLE window of rows with their visible
    // cells. Windowing is the whole reason a 10000x8 grid stays one widget, and
    // the container carries the true totals so a provider can still advertise
    // the real set size (§4.2 of plans/accessibility.md).
    void emit_grid(WalkCtx& c, uint32_t slot, const A11yNodeId& parent,
                   const Clip& clip, size_t ci)
    {
      using namespace neui_detail;
      Session& s = c.s;
      auto* gw = dynamic_cast<GridWidget*>(&s._widgets[slot]);
      if (!gw) return;
      GridModel& m = gw->model;

      const auto cfg = grid_read_config(gw->attrs.get());
      if (cfg.row_h <= 0) return;
      const GridViewport vp = grid_compute_viewport(m, gw->width, gw->height,
                                                    cfg.row_h, cfg.header_h);
      // Visual order has to be current or the reported rows would be in a
      // different order than the painted ones. This rebuilds the same cache the
      // paint path rebuilds; it is a cache, not user state.
      grid_ensure_sort_clean(m);

      const int n_rows = static_cast<int>(m.rows.size());
      const int n_cols = static_cast<int>(m.columns.size());
      const int body_x = gw->abs_x + vp.body_x;
      const int body_y = gw->abs_y + vp.body_y;

      // Which columns are on screen, in the same left-to-right sweep paint uses.
      struct ColRect { int col, x, w; };
      std::vector<ColRect> cols;
      {
        int cx = body_x - m.scroll_offset_x;
        for (int col = 0; col < n_cols; ++col) {
          const int cw = m.columns[static_cast<size_t>(col)].width;
          if (cx + cw < body_x) { cx += cw; continue; }
          if (cx > body_x + vp.body_w) break;
          cols.push_back(ColRect{ col, cx, cw });
          cx += cw;
        }
      }

      const Clip body_clip = clip_intersect(
        clip, Clip{ true, body_x, body_y, vp.body_w, vp.body_h });

      // Headers. Clipped to the header band, not the body - a horizontally
      // scrolled-away header is offscreen for the same reason its cells are.
      if (vp.header_h > 0) {
        const Clip hdr_clip = clip_intersect(
          clip, Clip{ true, body_x, gw->abs_y, vp.body_w, vp.header_h });
        for (const auto& cr : cols) {
          A11yInput r;
          r.id     = make_id(s, slot, A11ySubKind::grid_header, cr.col);
          r.parent = parent;
          r.x = cr.x; r.y = gw->abs_y; r.w = cr.w; r.h = vp.header_h;
          apply_clip(r, hdr_clip);
          r.visible       = true;
          r.enabled       = gw->enabled;
          r.modal_blocked = c.modal_blocked;
          const std::string& h = m.columns[static_cast<size_t>(cr.col)].header;
          r.text = h.empty() ? nullptr : h.c_str();
          c.out.push_back(r);
        }
      }

      c.out[ci].total_child_count = n_rows;
      c.out[ci].first_child_index = m.scroll_offset_y;
      if (n_rows == 0 || cols.empty()) return;

      const int first = m.scroll_offset_y;
      int last = first + grid_visible_rows(vp, cfg.row_h) +
                 (m.scroll_px_offset != 0 ? 2 : 1);
      if (last > n_rows) last = n_rows;

      const bool cell_focus = cfg.cell_focus;

      for (int vrow = first; vrow < last; ++vrow) {
        const int row = grid_visual_to_logical(m, vrow);
        if (row < 0 || row >= n_rows) continue;
        const int ry = body_y - m.scroll_px_offset + (vrow - first) * cfg.row_h;

        A11yInput rr;
        // sub_index is the LOGICAL row, so a reference survives a re-sort - the
        // row the user selected is still the same row afterwards.
        rr.id     = make_id(s, slot, A11ySubKind::grid_row, row);
        rr.parent = parent;
        rr.x = body_x; rr.y = ry; rr.w = vp.body_w; rr.h = cfg.row_h;
        apply_clip(rr, body_clip);
        rr.visible       = true;
        rr.enabled       = gw->enabled;
        rr.modal_blocked = c.modal_blocked;
        rr.selected      = (row == m.selected_row);
        const auto& rd = m.rows[static_cast<size_t>(row)];
        // A row's name is its first cell: an AT announcing the row before
        // descending into it should say something identifying, and the leftmost
        // column is the conventional key.
        if (!rd.cells.empty() && !rd.cells[0].empty())
          rr.text = rd.cells[0].c_str();
        c.out.push_back(rr);
        const A11yNodeId row_id = rr.id;

        for (const auto& cr : cols) {
          A11yInput cell;
          cell.id     = make_id(s, slot, A11ySubKind::grid_cell,
                                row * kCellRowStride + cr.col);
          cell.parent = row_id;
          cell.x = cr.x; cell.y = ry; cell.w = cr.w; cell.h = cfg.row_h;
          apply_clip(cell, body_clip);
          cell.visible       = true;
          cell.modal_blocked = c.modal_blocked;
          // Per-cell enabled override, same lookup the paint dims with.
          const GridCellOverride* ov = grid_find_override(m, row, cr.col);
          cell.enabled = gw->enabled && !(ov && ov->has_enabled && !ov->enabled);
          // In cell-focus mode the cursor is a (row, col) pair, so the CELL is
          // the selected thing; in row-focus mode only the row is.
          cell.selected = cell_focus && row == m.selected_row &&
                          cr.col == m.selected_col;
          // An editable column is a cell the user can change; anything else is
          // read-only, and saying so stops an AT offering an edit that fails.
          cell.readonly = !m.columns[static_cast<size_t>(cr.col)].editable;
          if (cr.col < static_cast<int>(rd.cells.size()) &&
              !rd.cells[static_cast<size_t>(cr.col)].empty())
            cell.text = rd.cells[static_cast<size_t>(cr.col)].c_str();
          c.out.push_back(cell);
        }
      }
    }

    // TABVIEW chips. The chip rects are widget-local and are produced by paint,
    // which is why the whole walk depends on a frame having painted.
    void emit_tab_chips(WalkCtx& c, uint32_t slot, const A11yNodeId& parent,
                        const Clip& clip, size_t ci)
    {
      Session& s = c.s;
      auto* tv = dynamic_cast<TabViewWidget*>(&s._widgets[slot]);
      if (!tv) return;
      std::vector<uint32_t> pages;
      tv->collect_pages(pages);
      const int n = static_cast<int>(tv->chips.size());
      c.out[ci].total_child_count = static_cast<int>(pages.size());
      c.out[ci].first_child_index = 0;
      if (n == 0) return;

      for (int i = 0; i < n; ++i) {
        const auto& ch = tv->chips[static_cast<size_t>(i)];
        A11yInput r;
        r.id     = make_id(s, slot, A11ySubKind::tab_chip, i);
        r.parent = parent;
        r.x = tv->abs_x + static_cast<int>(ch.x);
        r.y = tv->abs_y + static_cast<int>(ch.y);
        r.w = static_cast<int>(ch.w);
        r.h = static_cast<int>(ch.h);
        apply_clip(r, clip);
        r.visible       = true;
        r.enabled       = tv->enabled;
        r.modal_blocked = c.modal_blocked;
        r.selected      = (i == tv->selected);
        // The chip label is the TABPAGE's text - the page owns the label, the
        // strip only draws it.
        if (i < static_cast<int>(pages.size()) && s._widgets.exists(pages[static_cast<size_t>(i)])) {
          const std::string& t = s._widgets[pages[static_cast<size_t>(i)]].text;
          if (!t.empty()) r.text = t.c_str();
        }
        c.out.push_back(r);
      }
    }

    // MENUBAR / POPUPMENU items. Session::collect_menu_elements owns the
    // geometry (it is built by the same mb_build_* calls paint and hit-test use)
    // and decides what is on screen at all - notably reporting nothing for a
    // native OS menu, which the platform already exposes to the AT itself.
    void emit_menu(WalkCtx& c, uint32_t slot, const A11yNodeId& parent)
    {
      Session& s = c.s;
      std::vector<Session::MenuElementRect> elems;
      s.collect_menu_elements(slot, elems);
      if (elems.empty()) return;

      // The menu container itself. It is created 0x0 and carries no geometry of
      // its own, which the shared model tolerates for exactly this role pair.
      A11yInput container;
      container.id     = make_id(s, slot, A11ySubKind::widget, -1);
      container.parent = parent;
      container.type   = s._widgets[slot].type;
      container.visible       = true;
      container.enabled       = s._widgets[slot].enabled;
      container.modal_blocked = c.modal_blocked;
      read_declarations(s._widgets[slot].attrs.get(), container);
      c.out.push_back(container);

      for (const auto& e : elems) {
        A11yInput r;
        r.id     = make_id(s, slot, A11ySubKind::menu_item,
                           static_cast<int32_t>(e.item_id));
        r.parent = (e.parent_item != 0)
                     ? make_id(s, slot, A11ySubKind::menu_item,
                               static_cast<int32_t>(e.parent_item))
                     : container.id;
        r.x = e.x; r.y = e.y; r.w = e.w; r.h = e.h;
        r.visible       = true;
        r.enabled       = e.enabled;
        r.modal_blocked = c.modal_blocked;
        r.expandable    = e.has_submenu;
        r.expanded      = e.expanded;
        r.text          = e.text;
        // A separator has nothing to say and nothing to activate. Declaring it
        // decorative removes it here rather than leaving an AT to step over an
        // unnamed row.
        if (e.separator) r.declared_role = NEUI_A11Y_ROLE_NONE;
        // A checkable item reports CHECKED; an unchecked plain command must NOT
        // report "unchecked", which would imply it is a toggle.
        if (e.checked) r.check_state = NEUI_CHECK_CHECKED;
        c.out.push_back(r);
      }
    }

    // ---- The walk -----------------------------------------------------------

    void walk_children(WalkCtx& c, uint32_t parent_slot,
                       const A11yNodeId& parent_id, const Clip& clip)
    {
      Session& s = c.s;
      uint32_t idx = s._widgets.child(parent_slot);
      while (idx != 0) {
        const uint32_t next = s._widgets.next(idx);
        if (!s._widgets.exists(idx)) { idx = next; continue; }
        WidgetData& wd = s._widgets[idx];

        // Menu models have no place in the geometric walk: they are never
        // painted in the child pass, own no rect, and their items are positioned
        // by the cascade rather than by the tree.
        if (wd.is_menu_model()) {
          emit_menu(c, idx, parent_id);
          idx = next;
          continue;
        }

        A11yInput row = widget_row(c, idx, parent_id, clip);
        const A11yNodeId self = row.id;
        const size_t ci = c.out.size();
        c.out.push_back(row);

        if (type_is(wd, NEUI_W_LISTBOX) || type_is(wd, NEUI_W_COMBOBOX))
          emit_list_rows(c, idx, self, clip, ci);
        else if (type_is(wd, NEUI_W_TREEVIEW))
          emit_tree_items(c, idx, self, clip, ci);
        else if (type_is(wd, NEUI_W_GRID))
          emit_grid(c, idx, self, clip, ci);
        else if (type_is(wd, NEUI_W_TABVIEW))
          emit_tab_chips(c, idx, self, clip, ci);

        // Descend only into visible containers, matching both the paint walk and
        // refresh_abs_positions - an invisible parent's descendants have no
        // meaningful cached position, and the shared model prunes the subtree
        // anyway.
        if (wd.visible) {
          Clip child_clip = clip;
          // A SECTION / TABPAGE / TABVIEW clips its children to its body rect.
          // Supplying the clip is also what makes a scrolling SECTION derive as
          // a scroll area rather than a plain group, so it must only be supplied
          // where the paint walk really pushes one.
          const auto* slay = wd.section_layout_ptr();
          if (slay && slay->body_w > 0 && slay->body_h > 0) {
            child_clip = clip_intersect(child_clip,
              Clip{ true, wd.abs_x + slay->body_x, wd.abs_y + slay->body_y,
                    slay->body_w, slay->body_h });
          }
          walk_children(c, idx, self, child_clip);
        }
        idx = next;
      }
    }

    // Is this frame input-blocked by a modal dialog it owns? Mirrors the
    // platform_set_window_enabled(owner, false) in widget_show: there is no way
    // to ask the OS, so the same condition is re-derived from the dialog side.
    bool frame_modal_blocked(Session& s, uint32_t frame_index)
    {
      uint32_t idx = s._widgets.child(0);
      while (idx != 0) {
        if (s._widgets.exists(idx)) {
          WidgetData& wd = s._widgets[idx];
          if (wd.is_dialog() && wd.owner_index == frame_index && wd.visible) {
            if (auto* fw = dynamic_cast<FrameWidget*>(&wd))
              if (fw->modal_pump_active()) return true;
          }
        }
        idx = s._widgets.next(idx);
      }
      return false;
    }
  } // namespace

  A11yNodeId a11y_widget_node_id(Session& s, uint32_t widget_index)
  {
    return make_id(s, widget_index, A11ySubKind::widget, -1);
  }

  uint32_t a11y_slot_of_node_id(Session& s, const A11yNodeId& id)
  {
    if (id.widget_id == 0) return 0;
    if (((id.widget_id >> 16) & 0xffff) != (s.get_session_id() & 0xffff)) return 0;
    const uint32_t slot = id.widget_id & 0xffff;
    if (!s._widgets.exists(slot)) return 0;
    // The generation is the point: a live widget in a recycled slot is NOT the
    // widget this id was made for, and answering with it would be a wrong answer
    // rather than a missing one.
    if (s.a11y_generation(slot) != id.generation) return 0;
    return slot;
  }

  bool a11y_collect_frame_inputs(Session& s, uint32_t frame_index,
                                 std::vector<A11yInput>& out)
  {
    if (frame_index == 0 || !s._widgets.exists(frame_index)) return false;
    if (!s._widgets[frame_index].is_frame()) return false;
    // Sub-element geometry (SECTION / TABVIEW body rects, TABVIEW chips) exists
    // only as a by-product of painting, so a frame that has not painted since
    // its last change cannot be described. This forces one paint and reports
    // failure if it could not happen, leaving the cached geometry alone.
    if (!s.ensure_abs_positions(frame_index)) return false;

    WidgetData& fw = s._widgets[frame_index];
    WalkCtx c{ s, out, frame_modal_blocked(s, frame_index) };

    // The frame itself is the root: parent is the null id.
    A11yInput root;
    root.id     = make_id(s, frame_index, A11ySubKind::widget, -1);
    root.type   = fw.type;
    root.x = 0; root.y = 0; root.w = fw.width; root.h = fw.height;
    root.visible       = fw.visible;
    root.enabled       = fw.enabled;
    root.focused       = (frame_index == s._focused_widget);
    root.modal_blocked = c.modal_blocked;
    root.text          = fw.text.empty() ? nullptr : fw.text.c_str();
    read_declarations(fw.attrs.get(), root);
    out.push_back(root);

    // Children start below any in-frame menubar band, exactly as the paint walk
    // and refresh_abs_positions do.
    walk_children(c, frame_index, root.id, Clip{});
    return true;
  }

  std::vector<A11yNode> a11y_build_frame_tree(Session& s, uint32_t frame_index)
  {
    std::vector<A11yInput> in;
    if (!a11y_collect_frame_inputs(s, frame_index, in))
      return std::vector<A11yNode>();
    return neui_detail::build_a11y_tree(in);
  }

  // The session table lives in host.cpp; same `extern` the widget API uses.
  extern std::vector<std::unique_ptr<Session>> sessions;

  Session* a11y_live_session(uint32_t session_id)
  {
    if (session_id == 0 || session_id > sessions.size()) return nullptr;
    return sessions[session_id - 1].get();
  }

  bool a11y_frame_is_live(Session& s, uint32_t frame_index,
                          uint32_t frame_generation)
  {
    if (frame_index == 0 || !s._widgets.exists(frame_index)) return false;
    if (!s._widgets[frame_index].is_frame()) return false;
    // The instance id comes from a process-wide counter, so a recycled slot
    // never matches - which is the whole reason this takes a generation rather
    // than just testing existence.
    return s.a11y_generation(frame_index) == frame_generation;
  }

  std::vector<A11yNode> a11y_build_tree_for_frame(neui_widget_t frame)
  {
    if (frame.id == 0 || frame.id == widget_none.id)
      return std::vector<A11yNode>();
    const uint32_t session_id = (frame.id >> 16) & 0xffff;
    if (session_id == 0 || session_id > sessions.size())
      return std::vector<A11yNode>();
    Session* s = sessions[session_id - 1].get();
    if (!s) return std::vector<A11yNode>();
    return a11y_build_frame_tree(*s, frame.id & 0xffff);
  }
} // namespace xpl_host
