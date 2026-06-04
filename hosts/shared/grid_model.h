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

  // Smooth-scroll kinetics for pixel-precise vertical scrolling with inertial
  // momentum + elastic rubber-band (driven by hosts that have the rich wheel
  // data - macOS today). raw_px is the unbounded integral of scroll deltas in
  // logical pixels; the rubber-band map turns it into the committed position.
  // last_commit_px is the integer position last written to the model, used to
  // detect external (keyboard / drag / API) changes and resync. Inert on hosts
  // that don't drive it (win32 leaves the row-stepped scroll path untouched).
  struct GridScrollKinetics {
    double raw_px            = 0.0;
    int    last_commit_px    = 0;
    bool   suppress_momentum = false;
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
      int row = (row_h > 0) ? (m.scroll_offset_y + (content_y >= 0
                                ? content_y / row_h
                                : -((-content_y + row_h - 1) / row_h)))
                            : m.scroll_offset_y;
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
  // spring-back animation timer; this code owns the math + model mutation so
  // both hosts behave identically and the tuning lives in one place.

  // Spring-back lerp factor per 60 Hz tick (higher = snappier). Exponential
  // ease, so halving the per-tick decay roughly doubles the settle time.
  inline constexpr double GRID_SCROLL_BOUNCE_LERP = 0.29;
  // Below this pixel distance the spring-back snaps to target and stops.
  inline constexpr double GRID_SCROLL_BOUNCE_EPS = 0.5;
  // Hard cap on how far the content can elastically stretch past an edge
  // (logical px) - keeps the rubber-band tight on a hard flick.
  inline constexpr double GRID_SCROLL_OVERSCROLL_MAX = 60.0;
  // Stretch stiffness: smaller = more resistance (less travel per unit pull).
  inline constexpr double GRID_SCROLL_OVERSCROLL_STIFFNESS = 0.5;

  // WebKit-style rubber-band overshoot: maps an unbounded pull distance `x`
  // (>= 0) into a damped, asymptotically-bounded display distance. It
  // approaches `limit` (a capped fraction of the viewport) but never reaches
  // it, so the further you pull the less it gives.
  inline double grid_scroll_overshoot(double x, double dim)
  {
    double limit = dim * 0.5;
    if (limit > GRID_SCROLL_OVERSCROLL_MAX) limit = GRID_SCROLL_OVERSCROLL_MAX;
    if (limit <= 0.0) limit = 1.0;
    return (1.0 - 1.0 / (x * GRID_SCROLL_OVERSCROLL_STIFFNESS / limit + 1.0)) * limit;
  }

  // Map the unbounded raw scroll position to the elastic display position.
  // In-range values pass through; out-of-range values are damped.
  inline double grid_scroll_rubber(double raw, double max_px, double dim)
  {
    if (raw < 0.0)    return -grid_scroll_overshoot(-raw, dim);
    if (raw > max_px) return  max_px + grid_scroll_overshoot(raw - max_px, dim);
    return raw;
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

  // Platform-neutral description of a wheel event (the host fills this in from
  // its native event - NSEvent on macOS).
  struct GridWheelInput {
    double delta_px      = 0.0;    // logical px; subtracted from raw (natural scroll)
    bool   precise       = false;  // trackpad / Magic Mouse (vs classic wheel)
    bool   phase_began   = false;  // finger-down gesture started
    bool   phase_changed = false;  // finger-down gesture moving
    bool   phase_ended   = false;  // finger lifted / cancelled
    bool   momentum      = false;  // an inertial (post-lift) event
    bool   momentum_ended = false; // last inertial event
  };

  // Outcome of grid_scroll_wheel - tells the host how to drive its bounce
  // timer + repaint.
  struct GridWheelAction {
    bool stop_bounce  = false;   // a new gesture began - cancel any spring-back
    bool start_bounce = false;   // overscrolled + released - begin spring-back
    bool changed      = false;   // model scroll position changed - repaint
  };

  // Apply a wheel event to the kinetics + model. Mirrors the elastic logic:
  // free pixel scrolling in-range, damped stretch past the edges, and an
  // immediate spring-back when an inertial event runs into an edge (the
  // remaining momentum stream is then swallowed so it can't re-stretch).
  inline GridWheelAction grid_scroll_wheel(GridModel& m, const GridViewport& vp,
                                            int row_h, const GridWheelInput& in)
  {
    if (row_h <= 0) row_h = 1;
    auto& k = m.scroll_kin;
    GridWheelAction act;
    double max_px = grid_scroll_max_px(m, vp, row_h);

    // A new finger-down gesture cancels any spring-back + momentum suppression.
    if (in.phase_began) {
      act.stop_bounce = true;
      k.suppress_momentum = false;
    }

    // Already springing back from a momentum overscroll: swallow the rest of
    // the inertial stream so it can't re-stretch the edge mid-settle.
    if (in.momentum && k.suppress_momentum) {
      if (in.momentum_ended) k.suppress_momentum = false;
      return act;
    }

    // Resync the raw integral if something else moved the model (keyboard nav,
    // scrollbar drag, or the grid API) since the last commit.
    int committed = m.scroll_offset_y * row_h + m.scroll_px_offset;
    if (committed != k.last_commit_px) k.raw_px = (double)committed;

    k.raw_px -= in.delta_px;

    // Classic mouse wheel (no precise deltas, no gesture phase) doesn't
    // rubber-band on macOS - hard-clamp instead.
    bool legacy = !in.precise && !in.phase_began && !in.phase_changed &&
                  !in.phase_ended && !in.momentum;
    if (legacy) {
      if (k.raw_px < 0.0)         k.raw_px = 0.0;
      else if (k.raw_px > max_px) k.raw_px = max_px;
    }

    grid_scroll_commit(m, vp, row_h);
    act.changed = true;

    bool overscrolled = (k.raw_px < 0.0 || k.raw_px > max_px);
    bool finger_down  = in.phase_began || in.phase_changed;
    // Spring back the moment we're overscrolled and the user is no longer
    // dragging (finger lifted, or an inertial event hit the edge). Suppress
    // the residual momentum so the bounce isn't fought.
    if (overscrolled && !finger_down) {
      if (in.momentum) k.suppress_momentum = true;
      act.start_bounce = true;
    }
    if (in.momentum_ended) k.suppress_momentum = false;
    return act;
  }

  // One spring-back animation step (call at ~60 Hz). Returns true while still
  // animating, false once settled (the host stops its timer). Does NOT repaint.
  inline bool grid_scroll_bounce_step(GridModel& m, const GridViewport& vp,
                                       int row_h)
  {
    if (row_h <= 0) row_h = 1;
    auto& k = m.scroll_kin;
    // Abort if something else moved the scroll position since the last commit
    // (keyboard nav, scrollbar drag, or the grid API) - it owns the position
    // now, so the spring-back must yield rather than clobber it.
    int committed = m.scroll_offset_y * row_h + m.scroll_px_offset;
    if (committed != k.last_commit_px) return false;
    double max_px = grid_scroll_max_px(m, vp, row_h);
    double target = k.raw_px;
    if (target < 0.0)         target = 0.0;
    else if (target > max_px) target = max_px;
    double d = target - k.raw_px;
    if (std::fabs(d) < GRID_SCROLL_BOUNCE_EPS) {
      k.raw_px = target;
      grid_scroll_commit(m, vp, row_h);
      // NB: leave suppress_momentum set - the inertial stream can outlast the
      // spring-back; it is cleared on momentum-end / new gesture.
      return false;
    }
    k.raw_px += d * GRID_SCROLL_BOUNCE_LERP;
    grid_scroll_commit(m, vp, row_h);
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
