#pragma once

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include <neui/d/grid.h>

#include "attrs.h"
#include "edit_history.h"
#include "scroll_kinetics.h"
#include "scrollbar.h"
#include "text_edit.h"

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

  // Forward declarations for the sort helpers - grid_hit_test and
  // grid_ensure_row_visible call them but the bodies live further down in
  // this header (the sort engine block) so the inline definitions are below
  // their callers. The bodies stay inline; this is a pure ordering fix.
  struct GridModel;
  inline void grid_ensure_sort_clean(GridModel& m);
  inline int  grid_visual_to_logical(const GridModel& m, int vi);
  inline int  grid_logical_to_visual(const GridModel& m, int li);

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
    // Wheel kinetics selector. 0 (NEUI_GRID_SCROLL_PLATFORM) = host picks the
    // natural feel (macOS = smooth, Win32 / null = stepped). 1 = forced STEPPED,
    // 2 = forced SMOOTH. Anything else is treated as PLATFORM.
    int      scroll_mode     = 0;
  };

  // Resolve the effective scroll mode for the host. Each host passes its own
  // `platform_default_smooth` constant; PLATFORM (or unknown values) defer to it.
  inline bool grid_smooth_enabled(const GridPaintConfig& cfg,
                                    bool platform_default_smooth)
  {
    if (cfg.scroll_mode == NEUI_GRID_SCROLL_SMOOTH)  return true;
    if (cfg.scroll_mode == NEUI_GRID_SCROLL_STEPPED) return false;
    return platform_default_smooth;
  }

  struct GridColumn {
    std::string  header;
    int          width      = GRID_DEFAULT_NEW_COLUMN_W;
    int          min_width  = 0;    // 0 = use grid default
    GridColAlign align      = GridColAlign::Left;
    // Sorting. `sortable` gates user-driven header clicks (default true);
    // programmatic set_sort / add_sort always work. `sort_kind` selects how
    // the cell strings in this column are compared.
    bool                  sortable  = true;
    neui_grid_sort_kind_t sort_kind = NEUI_GRID_SORT_STRING;
    // Cell editing. When true, ENTER opens an in-place editor over the
    // selected cell in cell-focus mode. Default false. See d/grid.h.
    bool                  editable  = false;
  };

  // One level in a multi-column sort stack. dir is always ASC or DESC inside
  // the stack; NONE only appears in API calls and means "remove this level".
  struct GridSortLevel {
    int                  col;
    neui_grid_sort_dir_t dir;
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

  // In-place cell editor state. `active` gates everything else; when false
  // the editor is closed and the rest of the fields are stale. Logical
  // (row, col) so it survives sort changes. `te` is the reusable single-
  // line text buffer + cursor + selection (shared with future KNOB value
  // entry). `orig_text` is the cell's text at the moment editing began,
  // restored on ESC / commit-rejected. `history` is a per-edit-session
  // undo stack reset on grid_begin_edit.
  struct GridEditState {
    bool         active    = false;
    int          row       = -1;
    int          col       = -1;
    TextEditState te;
    std::string  orig_text;
    EditHistory  history;
  };

  // Smooth-scroll kinetics for pixel-precise vertical scrolling with inertial
  // momentum + elastic rubber-band (driven by hosts that have the rich wheel
  // data - macOS today). Generic primitive lives in `scroll_kinetics.h` and
  // is shared with the scrolling SECTION container; the GRID-specific
  // wrappers below (grid_scroll_wheel / _bounce_step / _commit) layer the
  // row-index + fine-px decomposition on top.
  using GridScrollKinetics = ScrollKinetics;

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

    // Fine vertical pixel offset applied on top of the row-indexed
    // scroll_offset_y, for sub-row-smooth scrolling + elastic overscroll.
    // The body content is shifted UP by this many logical pixels; a
    // negative value (top rubber-band) shifts it DOWN, leaving a blank
    // band above the first row. Default 0 -> pixel-exact row alignment.
    // Only the macOS host drives this non-zero (smooth wheel + momentum +
    // rubber-band); win32 / xpl leave it at 0, so their paint + hit-test
    // are unchanged.
    int scroll_px_offset = 0;

    // Column resize drag state. -1 = not dragging.
    int column_resize_col       = -1;
    int column_resize_start_x   = 0;
    int column_resize_start_w   = 0;
    int column_resize_old_w     = 0;   // captured at mousedown for the resize event payload

    // Vertical + horizontal scrollbar drag state.
    ScrollbarDrag vert_drag;
    ScrollbarDrag horz_drag;

    // Smooth-scroll / rubber-band kinetics (see GridScrollKinetics). Used by
    // the macOS hosts; inert elsewhere.
    GridScrollKinetics scroll_kin;

    // In-place cell editor (see GridEditState).
    GridEditState edit;

    // Multi-column sort. `sort_stack` is the active stack (level 0 = primary);
    // empty == unsorted (identity mapping). `display_order` maps visual row
    // index -> logical row index when a sort is active; empty when no sort
    // (so the existing paint / hit-test fast path stays unchanged).
    // `logical_to_visual` is the cached inverse, used by selection paint and
    // by the public logical_to_visual_row API. `sort_dirty` flips on any
    // mutation that could invalidate display_order; grid_ensure_sort_clean
    // rebuilds before paint / hit-test / public translation queries read it.
    std::vector<GridSortLevel> sort_stack;
    std::vector<int>           display_order;
    std::vector<int>           logical_to_visual;
    bool                       sort_dirty = false;
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
      // Fold in the fine pixel offset (content shifted up by scroll_px_offset)
      // so a partially-scrolled / overscrolled row maps to the right index.
      int content_y = local_y + m.scroll_px_offset;
      // Compute the VISUAL row first (position in the displayed list), then
      // translate to logical via display_order so callers always see logical
      // row indices. Callers are responsible for calling grid_ensure_sort_clean
      // before grid_hit_test so display_order is up-to-date.
      int vrow = (row_h > 0) ? (m.scroll_offset_y + (content_y >= 0
                                  ? content_y / row_h
                                  : -((-content_y + row_h - 1) / row_h)))
                              : m.scroll_offset_y;
      if (vrow < 0 || vrow >= (int)m.rows.size()) {
        hit.region = GridHitRegion::BodyEmpty;
        return hit;
      }
      int row = grid_visual_to_logical(m, vrow);
      if (row < 0) {
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

  // Ensure the selected row is fully visible after a cursor move. `row` is
  // LOGICAL (the public API contract); the function translates to its current
  // sort-order position before clamping scroll_offset_y, so a sorted grid
  // scrolls to the row the user would see in the sorted view.
  inline void grid_ensure_row_visible(GridModel& m,
                                       const GridViewport& vp, int row_h,
                                       int row)
  {
    if (row < 0) return;
    int vis = grid_visible_rows(vp, row_h);
    if (vis <= 0) return;
    grid_ensure_sort_clean(m);
    int vrow = grid_logical_to_visual(m, row);
    if (vrow < 0) return;
    if (vrow < m.scroll_offset_y)
      m.scroll_offset_y = vrow;
    else if (vrow >= m.scroll_offset_y + vis)
      m.scroll_offset_y = vrow - vis + 1;
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

  // ---- Stepped wheel path -------------------------------------------------
  // Row-quantized vertical scroll. The wheel delta is interpreted as a whole
  // number of rows (positive = scroll down / content moves up, matching the
  // wheel-line convention on both Win32 and macOS) and applied to the row-
  // indexed offset. Clamps hard at top / bottom and resets the smooth-scroll
  // kinetics so a later switch back to SMOOTH starts cleanly. Returns true
  // when the position changed.
  inline bool grid_scroll_step_rows(GridModel& m, const GridViewport& vp,
                                     int row_h, int row_step_signed)
  {
    if (row_h <= 0) row_h = 1;
    int before_y  = m.scroll_offset_y;
    int before_px = m.scroll_px_offset;
    m.scroll_offset_y += row_step_signed;
    m.scroll_px_offset = 0;
    grid_clamp_scroll(m, vp, row_h);
    // Resync the kinetics integrator so a later SMOOTH wheel event picks up
    // from the now-committed position instead of springing back to wherever
    // the integrator last was.
    m.scroll_kin.raw_px         = (double)(m.scroll_offset_y * row_h);
    m.scroll_kin.last_commit_px = m.scroll_offset_y * row_h;
    m.scroll_kin.suppress_momentum = true;
    return m.scroll_offset_y != before_y || m.scroll_px_offset != before_px;
  }

  // ---- Smooth scroll + elastic rubber-band --------------------------------
  // Pixel-precise vertical scrolling with inertial momentum + elastic
  // overscroll. Shared by every host that can feed rich wheel data (macOS
  // native + macOS xpl today). The host owns the event plumbing + the
  // spring-back animation timer; this header owns the GRID-specific commit
  // (raw-px -> row index + fine-px decomposition); the platform-neutral
  // kinetics + rubber-band math live in `scroll_kinetics.h` so the
  // scrolling SECTION container shares the same curve + tuning.

  // GRID_*-named aliases for the generic constants - tests reference them
  // directly + downstream tuning would land here first.
  inline constexpr double GRID_SCROLL_BOUNCE_LERP          = SCROLL_BOUNCE_LERP;
  inline constexpr double GRID_SCROLL_BOUNCE_EPS           = SCROLL_BOUNCE_EPS;
  inline constexpr double GRID_SCROLL_OVERSCROLL_MAX       = SCROLL_OVERSCROLL_MAX;
  inline constexpr double GRID_SCROLL_OVERSCROLL_STIFFNESS = SCROLL_OVERSCROLL_STIFFNESS;

  // GRID-named shims over the generic rubber-band primitives - tests +
  // existing call sites use these names.
  inline double grid_scroll_overshoot(double x, double dim)
  {
    return scroll_overshoot(x, dim);
  }
  inline double grid_scroll_rubber(double raw, double max_px, double dim)
  {
    return scroll_rubber(raw, max_px, dim);
  }

  // Maximum legal vertical scroll position in logical pixels (0 when the
  // content fits).
  inline double grid_scroll_max_px(const GridModel& m,
                                    const GridViewport& vp, int row_h)
  {
    double v = (double)((int)m.rows.size() * row_h - vp.body_h);
    return v < 0.0 ? 0.0 : v;
  }

  // Decompose the (rubber-mapped) raw position into the model's row index +
  // fine pixel offset. Records last_commit_px so the next wheel event can
  // detect external mutations. Does NOT repaint - the host does that.
  inline void grid_scroll_commit(GridModel& m, const GridViewport& vp, int row_h)
  {
    if (row_h <= 0) row_h = 1;
    double max_px = grid_scroll_max_px(m, vp, row_h);
    double pos = grid_scroll_rubber(m.scroll_kin.raw_px, max_px, (double)vp.body_h);
    if (pos <= 0.0) {
      m.scroll_offset_y  = 0;
      m.scroll_px_offset = (int)std::lround(pos);   // <= 0 -> top rubber-band gap
    } else {
      int row = (int)(pos / (double)row_h);
      m.scroll_offset_y  = row;
      m.scroll_px_offset = (int)std::lround(pos - (double)row * row_h);
    }
    m.scroll_kin.last_commit_px = m.scroll_offset_y * row_h + m.scroll_px_offset;
  }

  // Wheel event / action types - aliases for the generic primitives in
  // scroll_kinetics.h so existing callers (host wheel plumbing, tests)
  // keep their names.
  using GridWheelInput  = ScrollWheelInput;
  using GridWheelAction = ScrollWheelAction;

  // Apply a wheel event to the kinetics + GRID model. Thin GRID-side
  // wrapper over scroll_wheel that adds the row-index + fine-px commit
  // shape. Behaviour identical to the pre-extraction in-place version.
  inline GridWheelAction grid_scroll_wheel(GridModel& m, const GridViewport& vp,
                                            int row_h, const GridWheelInput& in)
  {
    if (row_h <= 0) row_h = 1;
    double max_px  = grid_scroll_max_px(m, vp, row_h);
    int committed  = m.scroll_offset_y * row_h + m.scroll_px_offset;
    GridWheelAction act = scroll_wheel(m.scroll_kin, in, max_px,
                                        (double)vp.body_h, committed);
    if (act.changed) grid_scroll_commit(m, vp, row_h);
    return act;
  }

  // One spring-back animation step (call at ~60 Hz). Returns true while
  // still animating, false once settled (the host stops its timer). Thin
  // GRID-side wrapper over scroll_bounce_step + grid_scroll_commit. If
  // something else moved the scroll position since the last commit
  // (keyboard nav, scrollbar drag, public API), the bounce yields without
  // re-committing so the external mutation isn't clobbered.
  inline bool grid_scroll_bounce_step(GridModel& m, const GridViewport& vp,
                                       int row_h)
  {
    if (row_h <= 0) row_h = 1;
    int committed = m.scroll_offset_y * row_h + m.scroll_px_offset;
    if (committed != m.scroll_kin.last_commit_px) return false;
    double max_px = grid_scroll_max_px(m, vp, row_h);
    bool animating = scroll_bounce_step(m.scroll_kin, max_px,
                                         (double)vp.body_h, committed);
    grid_scroll_commit(m, vp, row_h);
    return animating;
  }

  // ---- Sort engine --------------------------------------------------------
  // Multi-column sort over the row table. Public API row indices stay
  // LOGICAL; display_order provides the visual ordering. Comparator chain
  // walks sort_stack; std::stable_sort gives free tie-breaking by insertion
  // order so the result is deterministic.

  // Compare two cell strings under the given sort kind. Returns negative /
  // zero / positive like strcmp. Unparseable INT / FLOAT values group at one
  // end (negative numbers / negative compare against "real" values), so the
  // ASC caller naturally sorts them last by negating.
  inline int grid_compare_cells(const std::string& a, const std::string& b,
                                  neui_grid_sort_kind_t kind)
  {
    switch (kind) {
    case NEUI_GRID_SORT_INT: {
      const char* sa = a.c_str();
      const char* sb = b.c_str();
      char* ea = nullptr;
      char* eb = nullptr;
      long long va = std::strtoll(sa, &ea, 10);
      long long vb = std::strtoll(sb, &eb, 10);
      bool ok_a = (ea != sa);
      bool ok_b = (eb != sb);
      if (ok_a && ok_b) return (va < vb) ? -1 : (va > vb) ? 1 : 0;
      // Unparseable values sort last on ASC: parsed < unparsed.
      if (ok_a && !ok_b) return -1;
      if (!ok_a && ok_b) return 1;
      return std::strcmp(sa, sb);  // both unparseable - lexicographic
    }
    case NEUI_GRID_SORT_FLOAT: {
      const char* sa = a.c_str();
      const char* sb = b.c_str();
      char* ea = nullptr;
      char* eb = nullptr;
      double va = std::strtod(sa, &ea);
      double vb = std::strtod(sb, &eb);
      bool ok_a = (ea != sa);
      bool ok_b = (eb != sb);
      if (ok_a && ok_b) return (va < vb) ? -1 : (va > vb) ? 1 : 0;
      if (ok_a && !ok_b) return -1;
      if (!ok_a && ok_b) return 1;
      return std::strcmp(sa, sb);
    }
    case NEUI_GRID_SORT_NATURAL: {
      // Walk alternating digit / non-digit runs. Digit runs compared
      // numerically (with length tie-break for leading-zero stability),
      // non-digit runs lexicographically. Pure-ASCII; UTF-8 bytes outside
      // the digit class compare byte-wise.
      const unsigned char* pa = (const unsigned char*)a.c_str();
      const unsigned char* pb = (const unsigned char*)b.c_str();
      while (*pa && *pb) {
        bool da = (*pa >= '0' && *pa <= '9');
        bool db = (*pb >= '0' && *pb <= '9');
        if (da && db) {
          const unsigned char* sa = pa;
          const unsigned char* sb = pb;
          while (*pa >= '0' && *pa <= '9') ++pa;
          while (*pb >= '0' && *pb <= '9') ++pb;
          // Strip leading zeros for the numeric compare; remember original
          // lengths so a tied magnitude breaks on raw length (so "010" sorts
          // after "10" stably).
          const unsigned char* za = sa; while (za < pa && *za == '0') ++za;
          const unsigned char* zb = sb; while (zb < pb && *zb == '0') ++zb;
          ptrdiff_t la = pa - za;
          ptrdiff_t lb = pb - zb;
          if (la != lb) return (la < lb) ? -1 : 1;
          while (za < pa) {
            if (*za != *zb) return (*za < *zb) ? -1 : 1;
            ++za; ++zb;
          }
          ptrdiff_t orig_a = pa - sa;
          ptrdiff_t orig_b = pb - sb;
          if (orig_a != orig_b) return (orig_a < orig_b) ? -1 : 1;
          continue;
        }
        if (da != db) {
          // A digit run sorts before / after a non-digit run consistently;
          // pick digits-first so "Item 2" precedes "Item B".
          return da ? -1 : 1;
        }
        if (*pa != *pb) return (*pa < *pb) ? -1 : 1;
        ++pa; ++pb;
      }
      if (*pa) return 1;
      if (*pb) return -1;
      return 0;
    }
    case NEUI_GRID_SORT_STRING:
    default:
      return std::strcmp(a.c_str(), b.c_str());
    }
  }

  // Rebuild display_order + logical_to_visual from the current sort_stack.
  // Empty stack -> empty vectors (identity mapping). Stable across equal
  // keys. Clears sort_dirty.
  inline void grid_rebuild_display_order(GridModel& m)
  {
    m.sort_dirty = false;
    if (m.sort_stack.empty()) {
      m.display_order.clear();
      m.logical_to_visual.clear();
      return;
    }
    int n = (int)m.rows.size();
    m.display_order.resize((size_t)n);
    for (int i = 0; i < n; ++i) m.display_order[(size_t)i] = i;
    auto& stack = m.sort_stack;
    auto& cols  = m.columns;
    auto& rows  = m.rows;
    std::stable_sort(m.display_order.begin(), m.display_order.end(),
      [&](int a, int b) -> bool {
        for (const auto& lvl : stack) {
          int c = lvl.col;
          if (c < 0 || c >= (int)cols.size()) continue;
          const std::string& ca = (c < (int)rows[(size_t)a].cells.size())
                                    ? rows[(size_t)a].cells[(size_t)c]
                                    : std::string();
          const std::string& cb = (c < (int)rows[(size_t)b].cells.size())
                                    ? rows[(size_t)b].cells[(size_t)c]
                                    : std::string();
          int cmp = grid_compare_cells(ca, cb, cols[(size_t)c].sort_kind);
          if (cmp != 0) {
            return (lvl.dir == NEUI_GRID_SORT_ASC) ? (cmp < 0) : (cmp > 0);
          }
        }
        return false;  // equal under the whole stack - stable_sort preserves order
      });
    m.logical_to_visual.assign((size_t)n, -1);
    for (int v = 0; v < n; ++v)
      m.logical_to_visual[(size_t)m.display_order[(size_t)v]] = v;
  }

  // Rebuild display_order if needed. Cheap when clean; the public
  // logical_to_visual / paint / hit-test paths all call this before reading
  // the order so callers don't have to track sort_dirty themselves.
  inline void grid_ensure_sort_clean(GridModel& m)
  {
    if (m.sort_dirty) grid_rebuild_display_order(m);
  }

  // Visual row -> logical row. Identity when no sort is active.
  inline int grid_visual_to_logical(const GridModel& m, int vi)
  {
    if (m.display_order.empty()) return vi;
    if (vi < 0 || vi >= (int)m.display_order.size()) return -1;
    return m.display_order[(size_t)vi];
  }

  // Logical row -> visual row. Identity when no sort is active.
  inline int grid_logical_to_visual(const GridModel& m, int li)
  {
    if (m.logical_to_visual.empty()) return li;
    if (li < 0 || li >= (int)m.logical_to_visual.size()) return -1;
    return m.logical_to_visual[(size_t)li];
  }

  // Keyboard nav helpers. Per-host code routes Up / Down / PgUp / PgDn /
  // Home / End / Ctrl+Home / Ctrl+End through these so navigation operates
  // on VISUAL order (matching what the user sees) while selected_row stays
  // a stable logical index.

  // Current sort-position of selected_row, or -1 if no selection.
  inline int grid_selected_visual(const GridModel& m)
  {
    return grid_logical_to_visual(m, m.selected_row);
  }

  // Snap selected_row to the logical row at the given visual position,
  // clamping the visual position to [0, n_rows-1]. No-op on an empty grid
  // (selected_row -> -1). Does NOT fire events or touch scroll - the caller
  // owns those.
  inline void grid_set_selected_visual(GridModel& m, int vpos)
  {
    int n = (int)m.rows.size();
    if (n <= 0) { m.selected_row = -1; return; }
    if (vpos < 0) vpos = 0;
    if (vpos >= n) vpos = n - 1;
    m.selected_row = grid_visual_to_logical(m, vpos);
  }

  // Find a column's level in the sort stack, or -1 if not present.
  inline int grid_sort_stack_find(const GridModel& m, int col)
  {
    for (int i = 0; i < (int)m.sort_stack.size(); ++i)
      if (m.sort_stack[(size_t)i].col == col) return i;
    return -1;
  }

  // Gate for the user-driven header-click sort path: a click only cycles the
  // sort when `col` is a valid, sortable column. Every host's header-click
  // branch routes through this so the non-sortable rule can't drift between
  // platforms. Programmatic grid_set_sort / grid_add_sort intentionally do NOT
  // consult this - they bypass the `sortable` flag by design.
  inline bool grid_header_click_allowed(const GridModel& m, int col)
  {
    if (col < 0 || col >= (int)m.columns.size()) return false;
    return m.columns[(size_t)col].sortable;
  }

  // Apply a header click. shift_held selects Shift+click semantics:
  //   - Plain click on the ONLY sorted column: cycle asc -> desc -> empty.
  //   - Plain click otherwise: replace stack with [{col, ASC}].
  //   - Shift+click on a column already in the stack: cycle its level's dir
  //     (asc -> desc -> remove level), keeping the rest of the stack.
  //   - Shift+click on a new column: append {col, ASC}; if the stack is at
  //     NEUI_GRID_SORT_MAX_LEVELS, evict sort_stack[0] (FIFO) first.
  //   - Non-sortable columns are filtered by the caller.
  // Marks the model sort_dirty. Returns the new direction for `col` (or
  // NEUI_GRID_SORT_NONE if the level was just removed) so the caller can
  // populate the GRID_SORT_CHANGED event.
  inline neui_grid_sort_dir_t grid_apply_header_click(GridModel& m, int col,
                                                       bool shift_held)
  {
    auto cycle_next = [](neui_grid_sort_dir_t d) -> neui_grid_sort_dir_t {
      if (d == NEUI_GRID_SORT_ASC)  return NEUI_GRID_SORT_DESC;
      if (d == NEUI_GRID_SORT_DESC) return NEUI_GRID_SORT_NONE;
      return NEUI_GRID_SORT_ASC;
    };

    int existing = grid_sort_stack_find(m, col);
    neui_grid_sort_dir_t result = NEUI_GRID_SORT_ASC;

    if (!shift_held) {
      // Plain click. If the stack is exactly [this column], cycle its dir;
      // otherwise replace the stack with [{col, ASC}].
      if (m.sort_stack.size() == 1 && existing == 0) {
        neui_grid_sort_dir_t next = cycle_next(m.sort_stack[0].dir);
        if (next == NEUI_GRID_SORT_NONE) {
          m.sort_stack.clear();
          result = NEUI_GRID_SORT_NONE;
        } else {
          m.sort_stack[0].dir = next;
          result = next;
        }
      } else {
        m.sort_stack.clear();
        m.sort_stack.push_back({ col, NEUI_GRID_SORT_ASC });
        result = NEUI_GRID_SORT_ASC;
      }
    } else {
      // Shift+click.
      if (existing >= 0) {
        neui_grid_sort_dir_t next = cycle_next(m.sort_stack[(size_t)existing].dir);
        if (next == NEUI_GRID_SORT_NONE) {
          m.sort_stack.erase(m.sort_stack.begin() + existing);
          result = NEUI_GRID_SORT_NONE;
        } else {
          m.sort_stack[(size_t)existing].dir = next;
          result = next;
        }
      } else {
        if ((int)m.sort_stack.size() >= NEUI_GRID_SORT_MAX_LEVELS)
          m.sort_stack.erase(m.sort_stack.begin());  // FIFO evict oldest
        m.sort_stack.push_back({ col, NEUI_GRID_SORT_ASC });
        result = NEUI_GRID_SORT_ASC;
      }
    }
    m.sort_dirty = true;
    return result;
  }

  // Programmatic mutators. Mark sort_dirty + leave the stack to the caller
  // (these are the targets of the public API methods).
  inline void grid_set_sort(GridModel& m, int col, neui_grid_sort_dir_t dir)
  {
    m.sort_stack.clear();
    if (dir != NEUI_GRID_SORT_NONE && col >= 0)
      m.sort_stack.push_back({ col, dir });
    m.sort_dirty = true;
  }

  inline void grid_add_sort(GridModel& m, int col, neui_grid_sort_dir_t dir)
  {
    if (col < 0) return;
    int existing = grid_sort_stack_find(m, col);
    if (existing >= 0) {
      if (dir == NEUI_GRID_SORT_NONE) {
        m.sort_stack.erase(m.sort_stack.begin() + existing);
      } else {
        m.sort_stack[(size_t)existing].dir = dir;
      }
    } else if (dir != NEUI_GRID_SORT_NONE) {
      if ((int)m.sort_stack.size() >= NEUI_GRID_SORT_MAX_LEVELS)
        m.sort_stack.erase(m.sort_stack.begin());
      m.sort_stack.push_back({ col, dir });
    }
    m.sort_dirty = true;
  }

  inline void grid_clear_sort(GridModel& m)
  {
    m.sort_stack.clear();
    m.sort_dirty = true;
  }

  // After a column is removed, every sort_stack entry with col == removed
  // disappears and entries with col > removed shift down by one. After a
  // remove_column / clear_columns it is safest to drop sort_stack entirely
  // (the caller can pick this finer behaviour if it matters).
  inline void grid_sort_on_column_removed(GridModel& m, int removed_col)
  {
    for (auto it = m.sort_stack.begin(); it != m.sort_stack.end(); ) {
      if (it->col == removed_col) {
        it = m.sort_stack.erase(it);
      } else {
        if (it->col > removed_col) --it->col;
        ++it;
      }
    }
    m.sort_dirty = true;
  }

  // ---- Cell editor lifecycle ---------------------------------------------
  // The text-buffer primitives (insert / backspace / delete / move /
  // select_all / copy / cut / paste / undo / redo) live in text_edit.h and
  // operate on `m.edit.te.text` + `m.edit.te.cursor` + `m.edit.te.sel_anchor`.
  // These two helpers just frame the editing session.

  // Open the editor at (logical row, col). Caller must have validated:
  //   - column.editable is true
  //   - row / col in range
  //   - the cell is not disabled
  //   - the grid is in cell-focus mode (so a specific column is identified)
  // Resets the working buffer to the cell's current text and selects the
  // entire content (anchor=0, cursor=end) so the first character typed
  // replaces it (spreadsheet convention). Clears any prior undo history.
  inline void grid_begin_edit(GridModel& m, int row, int col)
  {
    if (row < 0 || row >= (int)m.rows.size()) return;
    if (col < 0 || col >= (int)m.columns.size()) return;
    const auto& rd = m.rows[(size_t)row];
    const std::string& cur = (col < (int)rd.cells.size())
                                ? rd.cells[(size_t)col]
                                : std::string();
    m.edit.active    = true;
    m.edit.row       = row;
    m.edit.col       = col;
    m.edit.te        = TextEditState{};
    m.edit.te.text   = cur;
    m.edit.te.sel_anchor = 0;
    m.edit.te.cursor     = (int)m.edit.te.text.size();
    m.edit.orig_text = cur;
    m.edit.history   = EditHistory{};
  }

  // Close the editor and return the working text. Caller passes it to the
  // validate callback / set_cell_text on commit, or discards on cancel.
  // Does NOT fire events.
  inline std::string grid_end_edit(GridModel& m)
  {
    std::string out = std::move(m.edit.te.text);
    m.edit = GridEditState{};
    return out;
  }

  // Gate for opening the editor: column editable, in range, cell not
  // disabled, cell_focus mode active. Used by hosts so the rules can't
  // drift between platforms.
  inline bool grid_cell_edit_allowed(const GridModel& m, int row, int col,
                                       bool cell_focus)
  {
    if (!cell_focus) return false;
    if (row < 0 || row >= (int)m.rows.size()) return false;
    if (col < 0 || col >= (int)m.columns.size()) return false;
    if (!m.columns[(size_t)col].editable) return false;
    const auto* ov = grid_find_override(m, row, col);
    if (ov && ov->has_enabled && !ov->enabled) return false;
    return true;
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
    c.scroll_mode     = bag->get_int(NEUI_ATTR_GRID_SCROLL_MODE,
                                       NEUI_GRID_SCROLL_PLATFORM);
    if (bag->has(NEUI_ATTR_BACKGROUND)) {
      c.bg_argb     = (uint32_t)bag->get_int(NEUI_ATTR_BACKGROUND, 0);
      c.bg_explicit = true;
    }
    return c;
  }

} // namespace neui_detail
