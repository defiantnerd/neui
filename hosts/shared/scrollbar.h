#pragma once

#include <algorithm>
#include <cstdint>

// Shared scrollbar geometry + drag-state helpers used by GRID (which has
// two scrollbars on one widget). The xpl LISTBOX / TREEVIEW still inline
// the same arithmetic; this header is a clean extraction point if those
// widgets are migrated later.
//
// Conventions:
//   - All coordinates are logical pixels at 96 DPI, widget-local
//     (origin at the widget's top-left).
//   - SCROLLBAR_W = 10 (matches the LISTBOX / TREEVIEW constant): 1 px
//     separator on the inside edge + 9 px track.
//   - Scroll position is stored as a signed int for both axes:
//     vertical = top row index, horizontal = pixel offset. Track + thumb
//     geometry use the same proportional formula regardless of axis;
//     callers translate "position" into "first-visible / start-px" as
//     suits their data model.

namespace neui_detail
{
  enum class ScrollAxis : uint8_t { Vert = 0, Horz = 1 };

  // Width of the scrollbar gutter (perpendicular to the axis). 1 px
  // separator on the inside edge + 9 px track.
  inline constexpr int SCROLLBAR_W   = 10;
  // Minimum thumb extent so tiny content with huge ranges still shows
  // a draggable handle. Matches the LISTBOX / TREEVIEW constant.
  inline constexpr int SCROLLBAR_MIN = 8;

  // Geometry of a thumb on its track, in logical pixels along the axis.
  struct ScrollbarGeom {
    bool visible    = false;  // false when content fits entirely in the viewport
    int  track_pos  = 0;      // start of usable track (axis coord)
    int  track_len  = 0;      // length of usable track (axis coord)
    int  thumb_pos  = 0;      // start of thumb on the track (axis coord)
    int  thumb_len  = 0;      // length of thumb (axis coord)
  };

  // Compute scrollbar thumb geometry from a viewport / content / position
  // triple. All values are in the same unit (rows for vert in LISTBOX
  // style, pixels for horz in GRID-body style).
  //
  //   viewport_axis  - widget extent along the axis (height for vert, width for horz)
  //   gutter_other   - the size occupied by the perpendicular scrollbar gutter
  //                    on the same edge (so a horizontal scrollbar leaves
  //                    space for the vertical one in the bottom-right corner).
  //                    Pass 0 when there is no perpendicular bar.
  //   content        - total content size along the axis (total rows for vert,
  //                    total pixels for horz)
  //   visible        - portion of content currently visible (rows visible
  //                    for vert, pixels of viewport for horz)
  //   position       - current scroll position (top row / left-px offset)
  inline ScrollbarGeom compute_scrollbar(int viewport_axis,
                                          int gutter_other,
                                          int content,
                                          int visible,
                                          int position)
  {
    ScrollbarGeom g{};
    if (viewport_axis <= 0 || content <= 0 || visible <= 0 || visible >= content) {
      // Content fits or invalid - no scrollbar.
      return g;
    }
    g.visible = true;
    // Track spans the axis minus 1 px padding on each end (matches existing
    // LISTBOX / TREEVIEW look). The opposite-axis gutter is subtracted from
    // the *end* of the track so the bottom-right corner becomes a dead square.
    g.track_pos = 1;
    g.track_len = viewport_axis - 2 - gutter_other;
    if (g.track_len < SCROLLBAR_MIN) g.track_len = SCROLLBAR_MIN;

    // Proportional thumb length, clamped.
    long long num = (long long)g.track_len * (long long)visible;
    int len = (int)(num / (long long)content);
    if (len < SCROLLBAR_MIN) len = SCROLLBAR_MIN;
    if (len > g.track_len)   len = g.track_len;
    g.thumb_len = len;

    int max_pos = content - visible;
    if (max_pos < 1) max_pos = 1;
    int travel = g.track_len - g.thumb_len;
    if (travel < 0) travel = 0;
    int p = position;
    if (p < 0)        p = 0;
    if (p > max_pos)  p = max_pos;
    long long offs = (long long)travel * (long long)p / (long long)max_pos;
    g.thumb_pos = g.track_pos + (int)offs;
    return g;
  }

  // Inverse of compute_scrollbar's proportional offset: given a thumb
  // coordinate (e.g. from a drag), return the corresponding content
  // position clamped to [0, content - visible].
  inline int scrollbar_pos_from_thumb(int thumb_axis_coord,
                                       const ScrollbarGeom& geom,
                                       int content, int visible)
  {
    if (!geom.visible || content <= visible) return 0;
    int travel = geom.track_len - geom.thumb_len;
    if (travel <= 0) return 0;
    int t = thumb_axis_coord - geom.track_pos;
    if (t < 0)       t = 0;
    if (t > travel)  t = travel;
    int max_pos = content - visible;
    long long p = (long long)t * (long long)max_pos / (long long)travel;
    if (p < 0)        p = 0;
    if (p > max_pos)  p = max_pos;
    return (int)p;
  }

  // Drag state for a single scrollbar. Caller initialises start_thumb_*
  // and start_position on mousedown over the thumb, then per mousemove
  // updates position via scrollbar_drag_apply.
  struct ScrollbarDrag {
    bool active            = false;
    int  start_axis_coord  = 0;   // mouse axis coord at drag start
    int  start_position    = 0;   // scroll position at drag start
  };

  // Apply a drag move: caller passes the current mouse axis-coordinate;
  // returns the new scroll position. No clamp is applied here beyond what
  // compute_scrollbar reads back - caller is expected to clamp on commit.
  inline int scrollbar_drag_apply(const ScrollbarDrag& drag,
                                   int current_axis_coord,
                                   const ScrollbarGeom& geom,
                                   int content, int visible)
  {
    if (!drag.active || !geom.visible) return drag.start_position;
    int travel = geom.track_len - geom.thumb_len;
    if (travel <= 0) return drag.start_position;
    int max_pos = content - visible;
    if (max_pos < 1) max_pos = 1;
    long long dpx = (long long)(current_axis_coord - drag.start_axis_coord);
    long long dp  = dpx * (long long)max_pos / (long long)travel;
    long long p   = (long long)drag.start_position + dp;
    if (p < 0)        p = 0;
    if (p > max_pos)  p = max_pos;
    return (int)p;
  }

} // namespace neui_detail
