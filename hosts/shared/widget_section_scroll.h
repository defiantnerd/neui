#pragma once

#include <cstdint>
#include <cstring>
#include <memory>

#include <neui/d/attrs.h>

#include "attrs.h"
#include "scroll_kinetics.h"
#include "scrollbar.h"
#include "widget_paint_section.h"

// Scrolling-SECTION runtime state + helpers. Shared by all three hosts
// (xpl + win32 native + macOS native); each host wires its own paint /
// event plumbing on top.
//
// The state struct is allocated lazily on WidgetData via unique_ptr; non-
// scrolling SECTIONs (the vast majority - and the historical default)
// pay nothing.

namespace neui_detail
{
  enum class SectionScrollAxis : uint8_t {
    None       = 0,
    Vertical   = 1,
    Horizontal = 2,
    Both       = 3,
  };

  inline SectionScrollAxis parse_section_scroll_mode(const char* s)
  {
    if (!s || !*s) return SectionScrollAxis::None;
    if (!std::strcmp(s, "vertical"))   return SectionScrollAxis::Vertical;
    if (!std::strcmp(s, "horizontal")) return SectionScrollAxis::Horizontal;
    if (!std::strcmp(s, "both"))       return SectionScrollAxis::Both;
    return SectionScrollAxis::None;
  }

  inline bool section_axis_has_v(SectionScrollAxis a)
  {
    return a == SectionScrollAxis::Vertical || a == SectionScrollAxis::Both;
  }
  inline bool section_axis_has_h(SectionScrollAxis a)
  {
    return a == SectionScrollAxis::Horizontal || a == SectionScrollAxis::Both;
  }

  // Per-SECTION runtime state. Allocated only when the section is
  // configured with NEUI_ATTR_SCROLL_MODE != "none".
  struct SectionScrollState {
    SectionScrollAxis axis = SectionScrollAxis::None;

    // Logical-px scroll offset. Content is shifted by (-scroll_x, -scroll_y).
    int scroll_x = 0;
    int scroll_y = 0;

    // Last-known content extent (cached at paint time so hit-test and
    // scrollbar drag can read it without rescanning children).
    int content_w = 0;
    int content_h = 0;

    // Active scrollbar drag (if any). Vertical and horizontal mutually
    // exclusive in practice but stored separately.
    ScrollbarDrag vert_drag;
    ScrollbarDrag horz_drag;

    // Smooth-scroll kinetics. Inert when the platform-stepped mode is in
    // effect; the host's wheel-handler decides whether to feed events
    // through these or hard-clamp them.
    ScrollKinetics kin_v;
    ScrollKinetics kin_h;

    // Per-section spring-back animation timer is host-owned. These flags
    // mirror the GRID pattern so a host that has timer infrastructure
    // knows when to start / stop it without re-computing.
    bool bounce_v_active = false;
    bool bounce_h_active = false;
  };

  // Logical-px layout of the section's painted regions.
  struct SectionLayout {
    int  band_h     = 0;   // header band height (0 if no chip)
    int  body_x     = 0;   // body rect (excludes scrollbar gutters)
    int  body_y     = 0;
    int  body_w     = 0;
    int  body_h     = 0;
    bool vert_sb_shown = false;
    bool horz_sb_shown = false;
  };

  // Compute the section's layout for a given widget size + scroll state +
  // content extent. `band_h` is the chip header band height in logical px
  // (0 when text empty / align "none"). The body rect is everything below
  // the band minus scrollbar gutters.
  inline SectionLayout compute_section_layout(int section_w, int section_h,
                                                int band_h,
                                                int content_w, int content_h,
                                                SectionScrollAxis axis)
  {
    SectionLayout L{};
    L.band_h = band_h;
    if (L.band_h > section_h) L.band_h = section_h;
    L.body_x = 0;
    L.body_y = L.band_h;
    L.body_w = section_w;
    L.body_h = section_h - L.band_h;
    if (L.body_w < 0) L.body_w = 0;
    if (L.body_h < 0) L.body_h = 0;

    // Mutually-recursive scrollbar visibility: a vertical bar steals body
    // width which may then require a horizontal bar (and vice versa). Two
    // passes converge immediately for the simple layouts the SECTION
    // produces today.
    bool wantV = section_axis_has_v(axis) && content_h > L.body_h;
    bool wantH = section_axis_has_h(axis) && content_w > L.body_w;
    for (int i = 0; i < 2; ++i) {
      int bw = L.body_w - (wantV ? SCROLLBAR_W : 0);
      int bh = L.body_h - (wantH ? SCROLLBAR_W : 0);
      if (bw < 0) bw = 0;
      if (bh < 0) bh = 0;
      wantV = section_axis_has_v(axis) && content_h > bh;
      wantH = section_axis_has_h(axis) && content_w > bw;
    }
    if (wantV) L.body_w -= SCROLLBAR_W;
    if (wantH) L.body_h -= SCROLLBAR_W;
    if (L.body_w < 0) L.body_w = 0;
    if (L.body_h < 0) L.body_h = 0;
    L.vert_sb_shown = wantV;
    L.horz_sb_shown = wantH;
    return L;
  }

  // Clamp scroll position to [0, max(0, content - body)] on each axis.
  // Returns true if any axis changed.
  inline bool clamp_section_scroll(SectionScrollState& st,
                                     int content_w, int content_h,
                                     int body_w, int body_h)
  {
    int max_x = content_w - body_w; if (max_x < 0) max_x = 0;
    int max_y = content_h - body_h; if (max_y < 0) max_y = 0;
    int old_x = st.scroll_x, old_y = st.scroll_y;
    if (st.scroll_x < 0)     st.scroll_x = 0;
    if (st.scroll_x > max_x) st.scroll_x = max_x;
    if (st.scroll_y < 0)     st.scroll_y = 0;
    if (st.scroll_y > max_y) st.scroll_y = max_y;
    return st.scroll_x != old_x || st.scroll_y != old_y;
  }

  // Resolve the scrollable content extent from the AttrBag + an auto-
  // bounding callback. Auto = max(child.x + child.width) / .y + .height
  // across direct children; the explicit content_width / content_height
  // attrs override when non-zero.
  template <typename AutoFn>
  inline void resolve_section_content_extent(const AttrBag* bag, AutoFn autofn,
                                              int body_w, int body_h,
                                              int& out_w, int& out_h)
  {
    int auto_w = 0, auto_h = 0;
    autofn(auto_w, auto_h);
    int cw = bag ? bag->get_int(NEUI_ATTR_CONTENT_WIDTH,  0) : 0;
    int ch = bag ? bag->get_int(NEUI_ATTR_CONTENT_HEIGHT, 0) : 0;
    if (cw <= 0) cw = auto_w;
    if (ch <= 0) ch = auto_h;
    if (cw < body_w) cw = body_w;
    if (ch < body_h) ch = body_h;
    out_w = cw;
    out_h = ch;
  }

  // Paint the section's scrollbars (if any). The body fill + chip have
  // already been painted by paint_section. Coordinates are widget-local
  // (caller has pushed a translate to the section's origin).
  //
  // Layout matches GRID: 1 px separator on the inside edge + 9 px track
  // + 7 px thumb centered (1 px padding either side). When both bars
  // show, the bottom-right corner gets a final dead-square paint that
  // overlays whatever bar pixels extend into it; this way the thumb at
  // max scroll visually reaches the end of its track (the corner
  // overlays the tail) rather than stopping a gutter-width short.
  inline void paint_section_scrollbars(neui_render_backend_t* backend,
                                        neui_render_ctx_t ctx,
                                        const SectionLayout& L,
                                        const SectionScrollState& st,
                                        uint32_t sep_argb,
                                        uint32_t track_argb,
                                        uint32_t thumb_argb)
  {
    if (!backend || !ctx) return;
    if (L.vert_sb_shown) {
      float sx = (float)(L.body_x + L.body_w);
      float sy = (float)L.body_y;
      backend->fill_rect(ctx, sx, sy, 1.0f, (float)L.body_h, sep_argb);
      backend->fill_rect(ctx, sx + 1.0f, sy,
                          (float)(SCROLLBAR_W - 1), (float)L.body_h, track_argb);
      ScrollbarGeom g = compute_scrollbar(L.body_h, 0,
                                            st.content_h, L.body_h, st.scroll_y);
      if (g.visible) {
        backend->fill_rect(ctx, sx + 2.0f, sy + (float)g.thumb_pos,
                            (float)(SCROLLBAR_W - 3),
                            (float)g.thumb_len, thumb_argb);
      }
    }
    if (L.horz_sb_shown) {
      float sx = (float)L.body_x;
      float sy = (float)(L.body_y + L.body_h);
      backend->fill_rect(ctx, sx, sy, (float)L.body_w, 1.0f, sep_argb);
      backend->fill_rect(ctx, sx, sy + 1.0f, (float)L.body_w,
                          (float)(SCROLLBAR_W - 1), track_argb);
      ScrollbarGeom g = compute_scrollbar(L.body_w, 0,
                                            st.content_w, L.body_w, st.scroll_x);
      if (g.visible) {
        backend->fill_rect(ctx, sx + (float)g.thumb_pos, sy + 2.0f,
                            (float)g.thumb_len,
                            (float)(SCROLLBAR_W - 3), thumb_argb);
      }
    }
    if (L.vert_sb_shown && L.horz_sb_shown) {
      backend->fill_rect(ctx,
                          (float)(L.body_x + L.body_w),
                          (float)(L.body_y + L.body_h),
                          (float)SCROLLBAR_W,
                          (float)SCROLLBAR_W,
                          sep_argb);
    }
  }

  // Hit-test the scrollbar areas. Returns 1 for vertical hit, 2 for
  // horizontal hit, 0 otherwise. Caller compares (x, y) widget-local.
  inline int section_scrollbar_hit(const SectionLayout& L,
                                     int local_x, int local_y)
  {
    if (L.vert_sb_shown) {
      int vx = L.body_x + L.body_w;
      if (local_x >= vx && local_x < vx + SCROLLBAR_W &&
          local_y >= L.body_y && local_y < L.body_y + L.body_h)
        return 1;
    }
    if (L.horz_sb_shown) {
      int hy = L.body_y + L.body_h;
      if (local_y >= hy && local_y < hy + SCROLLBAR_W &&
          local_x >= L.body_x && local_x < L.body_x + L.body_w)
        return 2;
    }
    return 0;
  }

  // Apply a wheel event to the section's scroll state. axis_h = true to
  // route the delta to the horizontal axis (e.g. Shift+wheel). Returns
  // true if the position changed.
  inline bool section_apply_wheel(SectionScrollState& st,
                                    const SectionLayout& L,
                                    double delta_px, bool axis_h)
  {
    if (axis_h && section_axis_has_h(st.axis)) {
      int max_x = st.content_w - L.body_w; if (max_x < 0) max_x = 0;
      int before = st.scroll_x;
      st.scroll_x += (int)delta_px;
      if (st.scroll_x < 0)     st.scroll_x = 0;
      if (st.scroll_x > max_x) st.scroll_x = max_x;
      return st.scroll_x != before;
    }
    if (!axis_h && section_axis_has_v(st.axis)) {
      int max_y = st.content_h - L.body_h; if (max_y < 0) max_y = 0;
      int before = st.scroll_y;
      st.scroll_y += (int)delta_px;
      if (st.scroll_y < 0)     st.scroll_y = 0;
      if (st.scroll_y > max_y) st.scroll_y = max_y;
      return st.scroll_y != before;
    }
    return false;
  }

} // namespace neui_detail
