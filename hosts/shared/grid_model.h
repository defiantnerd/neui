#pragma once

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <neui/d/grid.h>

#include "attrs.h"
#include "scrollbar.h"

// Shared GRID widget state model. Header-only / inline so both the xpl
// host and the native hosts can reuse the same data structures and
// helper math through `hosts/shared/widget_paint_grid.h`.
//
// Cells are NOT widgets - per-row + per-cell state lives directly on
// the grid widget so a 10000 x 8 grid is one widget and ~10000 row
// vectors, not 80000 widgets. The widget-id slot space is 16-bit per
// session so per-cell widgets aren't representable anyway.

namespace neui_detail
{
  // Layout / paint constants. Mirror the conventions used by LISTBOX +
  // TREEVIEW so the GRID feels at home alongside them.
  inline constexpr int GRID_DEFAULT_ROW_H            = 22;
  inline constexpr int GRID_DEFAULT_HEADER_H         = 24;
  inline constexpr int GRID_DEFAULT_COLUMN_MIN_W     = 24;
  inline constexpr int GRID_COLUMN_RESIZE_HIT_PX     = 3;   // +-3 px around the column boundary
  inline constexpr int GRID_CELL_PAD_X               = 6;   // left/right text inset inside a cell
  inline constexpr int GRID_HEADER_PAD_X             = 6;
  inline constexpr int GRID_DEFAULT_NEW_COLUMN_W     = 100;
  inline constexpr int GRID_LEFT_RIGHT_STEP_PX_FALLBACK = 60;

  enum class GridColAlign : uint8_t { Left = 0, Center = 1, Right = 2 };

  // Runtime configuration resolved from the widget's AttrBag once per
  // paint / per dispatch pass. The fields mirror the well-known GRID
  // attrs but the struct is what hot paint / event code reads (one attr
  // bag scan instead of N attr lookups per event).
  struct GridPaintConfig {
    int row_h          = 22;   // GRID_DEFAULT_ROW_H, set explicitly below
    int header_h       = 24;   // GRID_DEFAULT_HEADER_H
    int col_min_w_def  = 24;   // GRID_DEFAULT_COLUMN_MIN_W
    uint32_t focus_row_color = 0;   // explicit override; 0 = use accent @ alpha
    bool     show_focus_row  = true;
    bool     cell_focus      = false;
    uint32_t bg_argb         = 0;
    bool     bg_explicit     = false;
  };

  struct GridColumn {
    std::string  header;
    int          width      = GRID_DEFAULT_NEW_COLUMN_W;
    int          min_width  = 0;    // 0 = use grid default
    GridColAlign align      = GridColAlign::Left;
  };

  // Per-cell sparse override. Key in the owning map is (row << 32) | col.
  struct GridCellOverride {
    uint32_t color       = 0;       // 0 = no override
    bool     has_color   = false;
    bool     enabled     = true;
    bool     has_enabled = false;
  };

  // Per-row data: one text per column plus an optional sparse override
  // set. cells.size() always equals the grid's column_count at the
  // moment the row was added; columns added later are filled with empty
  // strings on demand by ensure_row_cell_count.
  struct GridRow {
    std::vector<std::string> cells;
  };

  // Two scrollbars, two axes of scroll state, one cursor (row or row+col)
  // plus per-cell sparse overrides. The struct stays a POD-of-collections
  // so it can live inline on a widget without extra heap.
  struct GridModel {
    std::vector<GridColumn> columns;
    std::vector<GridRow>    rows;

    // Sparse (row, col) -> override map. Keyed by uint64_t (row << 32 | col).
    std::unordered_map<uint64_t, GridCellOverride> cell_overrides;

    int selected_row = -1;
    int selected_col = -1;     // only meaningful when cell_focus is true

    // Scroll state. scroll_offset_y is row-indexed (top visible row);
    // scroll_offset_x is pixel-indexed inside the body content area.
    int scroll_offset_y = 0;
    int scroll_offset_x = 0;

    // Column resize drag state. -1 = not dragging.
    int column_resize_col       = -1;
    int column_resize_start_x   = 0;
    int column_resize_start_w   = 0;
    int column_resize_old_w     = 0;   // captured at mousedown for the resize event payload

    // Vertical + horizontal scrollbar drag state.
    ScrollbarDrag vert_drag;
    ScrollbarDrag horz_drag;
  };

  // ---- Cell-override key helpers ------------------------------------------

  inline uint64_t grid_cell_key(int row, int col)
  {
    return ((uint64_t)(uint32_t)row << 32) | (uint64_t)(uint32_t)col;
  }

  inline GridCellOverride* grid_find_override(GridModel& m, int row, int col)
  {
    auto it = m.cell_overrides.find(grid_cell_key(row, col));
    return (it == m.cell_overrides.end()) ? nullptr : &it->second;
  }
  inline const GridCellOverride* grid_find_override(const GridModel& m, int row, int col)
  {
    auto it = m.cell_overrides.find(grid_cell_key(row, col));
    return (it == m.cell_overrides.end()) ? nullptr : &it->second;
  }

  inline GridCellOverride& grid_ensure_override(GridModel& m, int row, int col)
  {
    return m.cell_overrides[grid_cell_key(row, col)];
  }

  // Drop the override entry if it is back to "no overrides" (so the
  // sparse map stays lean). Called after every mutate-then-no-op flip.
  inline void grid_prune_override(GridModel& m, int row, int col)
  {
    auto it = m.cell_overrides.find(grid_cell_key(row, col));
    if (it == m.cell_overrides.end()) return;
    if (!it->second.has_color && !it->second.has_enabled)
      m.cell_overrides.erase(it);
  }

  inline void grid_remove_row_overrides(GridModel& m, int row)
  {
    for (auto it = m.cell_overrides.begin(); it != m.cell_overrides.end(); ) {
      int r = (int)(it->first >> 32);
      if (r == row) it = m.cell_overrides.erase(it);
      else          ++it;
    }
  }

  // After a column add / remove, ensure every row has exactly `n` cells.
  inline void grid_resize_rows_to_columns(GridModel& m, int n)
  {
    for (auto& r : m.rows) {
      if ((int)r.cells.size() < n) r.cells.resize((size_t)n);
      else if ((int)r.cells.size() > n) r.cells.resize((size_t)n);
    }
  }

  // ---- Layout queries ------------------------------------------------------

  // Total horizontal extent of all columns - the "content width" used by
  // the horizontal scrollbar and the resize-end clamp.
  inline int grid_total_content_width(const GridModel& m)
  {
    int total = 0;
    for (const auto& c : m.columns) total += c.width;
    return total;
  }

  // Pixel offset of column `col`'s left edge inside the data viewport
  // (before applying scroll_offset_x).
  inline int grid_column_left(const GridModel& m, int col)
  {
    int x = 0;
    int n = (int)m.columns.size();
    if (col > n) col = n;
    for (int i = 0; i < col; ++i) x += m.columns[i].width;
    return x;
  }

  // Resolve column min width: per-column override beats the grid default.
  inline int grid_column_min_width(const GridModel& m, int col, int grid_default)
  {
    if (col < 0 || col >= (int)m.columns.size()) return grid_default;
    int mw = m.columns[col].min_width;
    return (mw > 0) ? mw : grid_default;
  }

  // Resolve column alignment string -> enum.
  inline GridColAlign grid_parse_align(const char* s)
  {
    if (!s) return GridColAlign::Left;
    if (!std::strcmp(s, "center")) return GridColAlign::Center;
    if (!std::strcmp(s, "right"))  return GridColAlign::Right;
    return GridColAlign::Left;
  }

  // ---- Viewport / hit-test ------------------------------------------------

  // Body viewport is the widget rect minus the header band on top and
  // the scrollbar gutters on the right + bottom (when visible).
  struct GridViewport {
    int header_h        = 0;
    int body_x          = 0;
    int body_y          = 0;
    int body_w          = 0;     // excludes vertical scrollbar gutter
    int body_h          = 0;     // excludes horizontal scrollbar gutter
    bool vert_sb_shown  = false;
    bool horz_sb_shown  = false;
  };

  // Compute viewport given the widget's logical width / height and the
  // current row / column extents. The vertical / horizontal scrollbar
  // visibility decision is mutually recursive (a vertical bar steals
  // content width which may then require a horizontal bar); we resolve
  // it in two passes which converges immediately for v1's layout.
  inline GridViewport grid_compute_viewport(const GridModel& m,
                                              int widget_w, int widget_h,
                                              int row_h, int header_h)
  {
    GridViewport v;
    v.header_h = header_h;
    v.body_x   = 0;
    v.body_y   = header_h;
    int avail_w = widget_w;
    int avail_h = widget_h - header_h;
    if (avail_h < 0) avail_h = 0;

    int content_h_rows = (int)m.rows.size() * row_h;
    int content_w_px   = grid_total_content_width(m);

    auto need_vert = [&](int viewport_w, int viewport_h) {
      return content_h_rows > viewport_h;
    };
    auto need_horz = [&](int viewport_w, int viewport_h) {
      return content_w_px > viewport_w;
    };

    // First pass.
    bool vsb = need_vert(avail_w, avail_h);
    bool hsb = need_horz(vsb ? (avail_w - SCROLLBAR_W) : avail_w, avail_h);
    // Second pass - if horz appeared it steals body height, which may
    // now force a vert that wasn't needed before.
    int viewport_h2 = hsb ? (avail_h - SCROLLBAR_W) : avail_h;
    int viewport_w2 = vsb ? (avail_w - SCROLLBAR_W) : avail_w;
    if (!vsb && need_vert(viewport_w2, viewport_h2)) {
      vsb = true;
      viewport_w2 = avail_w - SCROLLBAR_W;
    }
    if (!hsb && need_horz(viewport_w2, viewport_h2)) {
      hsb = true;
      viewport_h2 = avail_h - SCROLLBAR_W;
    }

    v.vert_sb_shown = vsb;
    v.horz_sb_shown = hsb;
    v.body_w = viewport_w2 > 0 ? viewport_w2 : 0;
    v.body_h = viewport_h2 > 0 ? viewport_h2 : 0;
    return v;
  }

  // Hit-test outcome - one of these regions for any widget-local point.
  enum class GridHitRegion : uint8_t {
    None        = 0,
    Header,           // inside the header band, not over a divider
    HeaderDivider,    // +-GRID_COLUMN_RESIZE_HIT_PX of a column boundary inside header
    Cell,             // inside a body cell (row + col populated)
    BodyEmpty,        // body area below the last row
    VertScrollTrack,  // somewhere on the vertical scrollbar gutter
    VertScrollThumb,
    HorzScrollTrack,
    HorzScrollThumb,
    Corner            // bottom-right dead square between the two scrollbars
  };

  struct GridHit {
    GridHitRegion region = GridHitRegion::None;
    int row              = -1;
    int col              = -1;   // for HeaderDivider, col is the column *to the LEFT* of the divider
  };

  // Resolve a widget-local point (logical pixels). row_h, header_h, and
  // the per-widget viewport are passed in so the caller can reuse cached
  // values.
  inline GridHit grid_hit_test(const GridModel& m,
                                 const GridViewport& vp,
                                 int row_h,
                                 int widget_w, int widget_h,
                                 int lx, int ly)
  {
    GridHit hit;
    if (lx < 0 || lx >= widget_w || ly < 0 || ly >= widget_h) return hit;

    // Corner dead square (bottom-right where both scrollbars meet).
    if (vp.vert_sb_shown && vp.horz_sb_shown &&
        lx >= widget_w - SCROLLBAR_W && ly >= widget_h - SCROLLBAR_W) {
      hit.region = GridHitRegion::Corner;
      return hit;
    }

    // Vertical scrollbar (right edge of body).
    if (vp.vert_sb_shown && lx >= widget_w - SCROLLBAR_W) {
      // We don't classify thumb-vs-track here; the caller can compare
      // ly to the thumb geometry it computed for paint.
      hit.region = GridHitRegion::VertScrollTrack;
      return hit;
    }
    // Horizontal scrollbar (bottom edge of body, below the body area).
    if (vp.horz_sb_shown && ly >= widget_h - SCROLLBAR_W) {
      hit.region = GridHitRegion::HorzScrollTrack;
      return hit;
    }

    // Header band.
    if (ly < vp.header_h) {
      // Test each column boundary for a divider grab. The boundary lives
      // at (column_left[i] + columns[i].width) in unscrolled coords; subtract
      // scroll_offset_x for screen position.
      int n = (int)m.columns.size();
      int x_running = -m.scroll_offset_x;
      for (int i = 0; i < n; ++i) {
        x_running += m.columns[i].width;
        int divider_screen_x = x_running;
        if (divider_screen_x >= vp.body_w + GRID_COLUMN_RESIZE_HIT_PX) break;
        if (lx >= divider_screen_x - GRID_COLUMN_RESIZE_HIT_PX &&
            lx <= divider_screen_x + GRID_COLUMN_RESIZE_HIT_PX) {
          hit.region = GridHitRegion::HeaderDivider;
          hit.col    = i;
          return hit;
        }
      }
      // Determine which column the cursor sits over (informational).
      int xr = -m.scroll_offset_x;
      for (int i = 0; i < n; ++i) {
        if (lx >= xr && lx < xr + m.columns[i].width) {
          hit.col = i;
          break;
        }
        xr += m.columns[i].width;
      }
      hit.region = GridHitRegion::Header;
      return hit;
    }

    // Body.
    if (ly < vp.header_h + vp.body_h) {
      int local_y = ly - vp.header_h;
      int row = m.scroll_offset_y + (local_y / row_h);
      if (row < 0 || row >= (int)m.rows.size()) {
        hit.region = GridHitRegion::BodyEmpty;
        return hit;
      }
      // Which column does lx land in (in scrolled coords)?
      int n = (int)m.columns.size();
      int xr = -m.scroll_offset_x;
      int col = -1;
      for (int i = 0; i < n; ++i) {
        if (lx >= xr && lx < xr + m.columns[i].width) { col = i; break; }
        xr += m.columns[i].width;
      }
      if (col < 0) {
        // Cursor is past the last column - still inside the body but no cell.
        hit.region = GridHitRegion::BodyEmpty;
        hit.row    = row;
        return hit;
      }
      hit.region = GridHitRegion::Cell;
      hit.row    = row;
      hit.col    = col;
      return hit;
    }

    hit.region = GridHitRegion::None;
    return hit;
  }

  // ---- Cursor + scroll helpers --------------------------------------------

  // Number of fully visible body rows.
  inline int grid_visible_rows(const GridViewport& vp, int row_h)
  {
    if (row_h <= 0) return 0;
    return vp.body_h / row_h;
  }

  // Clamp scroll_offset_y / scroll_offset_x to the legal range.
  inline void grid_clamp_scroll(GridModel& m, const GridViewport& vp, int row_h)
  {
    int vis_rows = grid_visible_rows(vp, row_h);
    int max_y = (int)m.rows.size() - vis_rows;
    if (max_y < 0) max_y = 0;
    if (m.scroll_offset_y > max_y) m.scroll_offset_y = max_y;
    if (m.scroll_offset_y < 0)     m.scroll_offset_y = 0;

    int content_w = grid_total_content_width(m);
    int max_x = content_w - vp.body_w;
    if (max_x < 0) max_x = 0;
    if (m.scroll_offset_x > max_x) m.scroll_offset_x = max_x;
    if (m.scroll_offset_x < 0)     m.scroll_offset_x = 0;
  }

  // Ensure the selected row is fully visible after a cursor move.
  inline void grid_ensure_row_visible(GridModel& m,
                                       const GridViewport& vp, int row_h,
                                       int row)
  {
    if (row < 0) return;
    int vis = grid_visible_rows(vp, row_h);
    if (vis <= 0) return;
    if (row < m.scroll_offset_y)
      m.scroll_offset_y = row;
    else if (row >= m.scroll_offset_y + vis)
      m.scroll_offset_y = row - vis + 1;
    grid_clamp_scroll(m, vp, row_h);
  }

  inline void grid_ensure_cell_visible(GridModel& m,
                                        const GridViewport& vp, int row_h,
                                        int row, int col)
  {
    grid_ensure_row_visible(m, vp, row_h, row);
    if (col < 0 || col >= (int)m.columns.size()) return;
    int left_in_content = grid_column_left(m, col);
    int right_in_content = left_in_content + m.columns[col].width;
    if (left_in_content < m.scroll_offset_x)
      m.scroll_offset_x = left_in_content;
    else if (right_in_content > m.scroll_offset_x + vp.body_w)
      m.scroll_offset_x = right_in_content - vp.body_w;
    grid_clamp_scroll(m, vp, row_h);
  }

  // Pixel step used by Left/Right keys in row-focus mode (where the
  // cursor doesn't move per-cell). Picks the median column width, or
  // falls back to a sensible default if the grid is empty.
  inline int grid_horizontal_step_px(const GridModel& m)
  {
    if (m.columns.empty()) return GRID_LEFT_RIGHT_STEP_PX_FALLBACK;
    long long sum = 0;
    for (const auto& c : m.columns) sum += c.width;
    int avg = (int)(sum / (long long)m.columns.size());
    return avg > 0 ? avg : GRID_LEFT_RIGHT_STEP_PX_FALLBACK;
  }

  // Read NEUI_ATTR_GRID_* into a GridPaintConfig in one pass. Defaults
  // mirror the static constants above.
  inline GridPaintConfig grid_read_config(const AttrBag* bag)
  {
    GridPaintConfig c;
    c.row_h         = GRID_DEFAULT_ROW_H;
    c.header_h      = GRID_DEFAULT_HEADER_H;
    c.col_min_w_def = GRID_DEFAULT_COLUMN_MIN_W;
    if (!bag) return c;
    int rh = bag->get_int(NEUI_ATTR_GRID_ROW_HEIGHT, GRID_DEFAULT_ROW_H);
    if (rh > 0) c.row_h = rh;
    int hh = bag->get_int(NEUI_ATTR_GRID_HEADER_HEIGHT, GRID_DEFAULT_HEADER_H);
    if (hh >= 0) c.header_h = hh;
    int mw = bag->get_int(NEUI_ATTR_GRID_COLUMN_MIN_WIDTH_DEFAULT,
                          GRID_DEFAULT_COLUMN_MIN_W);
    if (mw > 0) c.col_min_w_def = mw;
    c.focus_row_color = (uint32_t)bag->get_int(NEUI_ATTR_GRID_FOCUS_ROW_COLOR, 0);
    c.show_focus_row  = bag->get_int(NEUI_ATTR_GRID_SHOW_FOCUS_ROW, 1) != 0;
    c.cell_focus      = bag->get_int(NEUI_ATTR_GRID_CELL_FOCUS, 0) != 0;
    if (bag->has(NEUI_ATTR_BACKGROUND)) {
      c.bg_argb     = (uint32_t)bag->get_int(NEUI_ATTR_BACKGROUND, 0);
      c.bg_explicit = true;
    }
    return c;
  }

} // namespace neui_detail
