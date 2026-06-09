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

    // Smooth-scroll kinetics, one integrator per axis. The host's wheel
    // plumbing feeds rich wheel input (NSEvent phases on macOS; synthetic
    // precise deltas on Win32) through section_scroll_wheel_kinetic below;
    // hosts without that detail fall back to section_apply_wheel.
    ScrollKinetics kin_v;
    ScrollKinetics kin_h;

    // True while the committed scroll position on this axis is an
    // intentional kinetic rubber-band overshoot (out of [0, max]).
    // Set / cleared by section_scroll_commit; the paint-time clamp skips
    // an axis while its flag is set so the stretch isn't snapped away
    // mid-gesture / mid-bounce.
    bool kinetic_over_v = false;
    bool kinetic_over_h = false;

    // Last position reported via NEUI_EVENT_SCROLL_CHANGED. The notify
    // helpers compare against these before firing so a wheel tick that
    // doesn't actually move the position (already at an edge, in-flight
    // bounce that rounded to the same px) stays silent. INT_MIN as the
    // initial sentinel means the first notify always fires.
    int last_notified_x = INT32_MIN;
    int last_notified_y = INT32_MIN;
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

  // Like clamp_section_scroll, but leaves an axis alone while its kinetics
  // own the current position as an intentional rubber-band overshoot
  // (kinetic_over_* set AND the committed value is exactly what the last
  // kinetic commit wrote). Any externally-caused out-of-range position
  // (content regenerated smaller, body resized) still clamps. Called from
  // the paint path, which runs on every frame of a stretch / bounce.
  inline bool clamp_section_scroll_idle(SectionScrollState& st,
                                          int content_w, int content_h,
                                          int body_w, int body_h)
  {
    int max_x = content_w - body_w; if (max_x < 0) max_x = 0;
    int max_y = content_h - body_h; if (max_y < 0) max_y = 0;
    bool skip_x = st.kinetic_over_h && st.scroll_x == st.kin_h.last_commit_px;
    bool skip_y = st.kinetic_over_v && st.scroll_y == st.kin_v.last_commit_px;
    int old_x = st.scroll_x, old_y = st.scroll_y;
    if (!skip_x) {
      if (st.scroll_x < 0)     st.scroll_x = 0;
      if (st.scroll_x > max_x) st.scroll_x = max_x;
    }
    if (!skip_y) {
      if (st.scroll_y < 0)     st.scroll_y = 0;
      if (st.scroll_y > max_y) st.scroll_y = max_y;
    }
    return st.scroll_x != old_x || st.scroll_y != old_y;
  }

  // Auto-bounding content extent over a parent's direct children. Tree<T>
  // must expose child(i) / next(i) / exists(i) and yield a T with x / y /
  // width / height / visible (the WidgetData shape on all three hosts).
  // Hidden children are skipped. Out params zeroed before the walk.
  template <typename TreeT>
  inline void section_compute_auto_extent(TreeT& widgets, uint32_t parent_idx,
                                            int& out_w, int& out_h)
  {
    out_w = 0;
    out_h = 0;
    uint32_t idx = widgets.child(parent_idx);
    while (idx != 0) {
      if (widgets.exists(idx)) {
        auto& cw = widgets[idx];
        if (cw.visible) {
          int rx = cw.x + cw.width;
          int by = cw.y + cw.height;
          if (rx > out_w) out_w = rx;
          if (by > out_h) out_h = by;
        }
      }
      idx = widgets.next(idx);
    }
  }

  // Header-band height for a section, mirroring paint_section's band
  // logic: 0 when text is empty or align "none", else SECTION_HEADER_H
  // clamped to fit the section's height.
  inline int section_band_h_for(const char* text, int height, const char* align)
  {
    if (!text || !*text) return 0;
    if (section_align_is_none(align)) return 0;
    int bh = static_cast<int>(SECTION_HEADER_H);
    if (bh > height) bh = height;
    return bh;
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

  // Read the section's kinetics-mode attr. Defaults to 0
  // (NEUI_SCROLL_KINETICS_PLATFORM) when unset; null bag returns 0.
  inline int section_read_kinetics_mode(const AttrBag* bag)
  {
    if (!bag) return 0;
    return bag->get_int(NEUI_ATTR_SCROLL_KINETICS, 0);
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

  // Compute the scroll offset needed to bring a body-local rect
  // (rect_x, rect_y, rect_w, rect_h) into the section's visible body
  // given the current scroll position. The rect is expressed in the same
  // body-local coordinate system the section's children use (i.e.
  // wd.x / wd.y for a direct child). Returns the new scroll_x / scroll_y
  // via out params. Minimum-motion policy:
  //   - axis fully visible inside [scroll, scroll + body] → no change
  //   - rect lies above / left of viewport → scroll up to the rect's edge
  //   - rect lies below / right of viewport → scroll down so the rect's
  //     trailing edge meets the body's trailing edge
  //   - rect larger than body → align to top-left (rect.x, rect.y)
  // Output is clamped to the legal scroll range; the caller compares
  // against current scroll_x / scroll_y to detect a no-op.
  inline void compute_ensure_visible(int rect_x, int rect_y,
                                       int rect_w, int rect_h,
                                       int body_w, int body_h,
                                       int content_w, int content_h,
                                       int cur_scroll_x, int cur_scroll_y,
                                       int& out_x, int& out_y)
  {
    int max_x = content_w - body_w; if (max_x < 0) max_x = 0;
    int max_y = content_h - body_h; if (max_y < 0) max_y = 0;

    int nx = cur_scroll_x;
    if (rect_w >= body_w) {
      nx = rect_x;
    } else if (rect_x < cur_scroll_x) {
      nx = rect_x;
    } else if (rect_x + rect_w > cur_scroll_x + body_w) {
      nx = rect_x + rect_w - body_w;
    }
    if (nx < 0)      nx = 0;
    if (nx > max_x)  nx = max_x;

    int ny = cur_scroll_y;
    if (rect_h >= body_h) {
      ny = rect_y;
    } else if (rect_y < cur_scroll_y) {
      ny = rect_y;
    } else if (rect_y + rect_h > cur_scroll_y + body_h) {
      ny = rect_y + rect_h - body_h;
    }
    if (ny < 0)      ny = 0;
    if (ny > max_y)  ny = max_y;

    out_x = nx;
    out_y = ny;
  }

  // Per-notch scale applied to the smooth-path wheel delta before committing
  // it in stepped mode. The smooth-path tuning (SECTION_WHEEL_LINE_PX = 40
  // px/line; on Win32 also multiplied by SPI_GETWHEELSCROLLLINES = 3 by
  // default) is sized for the damped + integrated kinetic feel where each
  // notch's full magnitude bleeds across many paint frames. Applied
  // verbatim to a discrete stepped commit it feels chunky - one Win32
  // wheel notch jumps 120 px (~5 button rows) before kinetics' damping
  // can do its job. Halving lands at ~60 px/notch on Win32 default,
  // matching GRID stepped's per-notch feel (3 rows * row_h = 66 px).
  inline constexpr double SECTION_STEPPED_SCALE = 0.5;

  // Hard-clamp stepped wheel step: drives a flat px delta into the chosen
  // axis with no rubber-band / momentum, then resyncs the kinetics
  // integrator so a later flip to SMOOTH picks up from the committed
  // position instead of springing back to wherever the integrator last
  // was. Refuses (returns false) when the section's axis mask doesn't
  // include the requested axis. SECTION twin of grid_scroll_step_rows.
  // Returns true on position change so callers know to repaint / reposition.
  //
  // Sign convention matches section_scroll_wheel_kinetic (positive delta =
  // scroll up = position decreases) so host wheel handlers can swap between
  // STEPPED + SMOOTH paths without re-deriving the delta. NB: this is the
  // opposite of `section_apply_wheel`'s legacy "position += delta"
  // convention, which is why STEPPED can't simply delegate to it. Callers
  // pass the same delta they would pass to section_scroll_wheel_kinetic;
  // this helper applies SECTION_STEPPED_SCALE internally so a discrete-
  // notch commit doesn't feel twice as coarse as the smooth path.
  inline bool section_scroll_step_px(SectionScrollState& st,
                                       const SectionLayout& L,
                                       double delta_px, bool axis_h)
  {
    if (axis_h ? !section_axis_has_h(st.axis) : !section_axis_has_v(st.axis))
      return false;
    delta_px *= SECTION_STEPPED_SCALE;
    int before  = axis_h ? st.scroll_x : st.scroll_y;
    double dim_max = axis_h ? (double)(st.content_w - L.body_w)
                            : (double)(st.content_h - L.body_h);
    int max_px = dim_max < 0.0 ? 0 : (int)dim_max;
    long long after = (long long)before - (long long)std::lround(delta_px);
    if (after < 0)              after = 0;
    if (after > (long long)max_px) after = max_px;
    int after_i = (int)after;
    if (axis_h) st.scroll_x = after_i; else st.scroll_y = after_i;
    ScrollKinetics& k = axis_h ? st.kin_h : st.kin_v;
    k.raw_px            = (double)after_i;
    k.last_commit_px    = after_i;
    k.suppress_momentum = true;
    if (axis_h) st.kinetic_over_h = false; else st.kinetic_over_v = false;
    return after_i != before;
  }

  // ---- Smooth-scroll kinetics (same curve + tuning as GRID) ----------------
  //
  // SECTION-side wrappers over the generic primitives in scroll_kinetics.h,
  // mirroring grid_model.h's grid_scroll_wheel / _bounce_step / _commit
  // shape. The SECTION commit is simpler than GRID's: the model position IS
  // a flat px offset, no row decomposition. Per-axis: axis_h = false drives
  // (kin_v, scroll_y), axis_h = true drives (kin_h, scroll_x).
  //
  // Host wiring is identical to GRID: rich wheel data (NSEvent phases /
  // precise deltas on macOS; synthetic precise notches on Win32) feeds
  // section_scroll_wheel_kinetic; a returned start_bounce starts the host's
  // 60 Hz timer driving section_scroll_bounce_step until it returns false.

  // Wheel speed for line-based wheel input (classic notch wheels + the
  // line-delta fallback path in the SECTION's own on_mouse_event): logical
  // px scrolled per wheel line.
  inline constexpr double SECTION_WHEEL_LINE_PX = 40.0;

  // Maximum legal scroll position on the axis (0 when the content fits).
  inline double section_scroll_max_px(const SectionScrollState& st,
                                        const SectionLayout& L, bool axis_h)
  {
    double v = axis_h ? (double)(st.content_w - L.body_w)
                      : (double)(st.content_h - L.body_h);
    return v < 0.0 ? 0.0 : v;
  }

  // Commit the kinetics' damped display position into the model's flat px
  // offset. `raw_px` is already the rubber-damped value (damping is applied
  // at input time inside scroll_wheel), so commit just rounds and routes
  // it. Records last_commit_px + the kinetic-overshoot flag so the paint-
  // time clamp and the next wheel event's external-mutation detection
  // both work. Does NOT repaint - the host does that.
  inline void section_scroll_commit(SectionScrollState& st,
                                      const SectionLayout& L, bool axis_h)
  {
    ScrollKinetics& k = axis_h ? st.kin_h : st.kin_v;
    double max_px = section_scroll_max_px(st, L, axis_h);
    double pos    = k.raw_px;
    int p = (int)std::lround(pos);
    bool over = pos < 0.0 || pos > max_px;
    if (axis_h) { st.scroll_x = p; st.kinetic_over_h = over; }
    else        { st.scroll_y = p; st.kinetic_over_v = over; }
    k.last_commit_px = p;
  }

  // Apply a rich wheel event to one axis' kinetics + commit. Thin wrapper
  // over scroll_wheel - behaviourally the SECTION twin of grid_scroll_wheel.
  // Refuses (no-op return) when the section's axis mask doesn't include
  // the requested axis - so a stray cross-axis wheel that slipped past
  // the per-host filter (tilt wheel / two-finger sideways trackpad scroll
  // on a vertical-only section, or the reverse) can't silently feed the
  // other axis' kinetics.
  inline ScrollWheelAction section_scroll_wheel_kinetic(SectionScrollState& st,
                                                          const SectionLayout& L,
                                                          const ScrollWheelInput& in,
                                                          bool axis_h)
  {
    if (axis_h ? !section_axis_has_h(st.axis) : !section_axis_has_v(st.axis))
      return {};
    ScrollKinetics& k = axis_h ? st.kin_h : st.kin_v;
    double max_px = section_scroll_max_px(st, L, axis_h);
    double dim    = axis_h ? (double)L.body_w : (double)L.body_h;
    int committed = axis_h ? st.scroll_x : st.scroll_y;
    ScrollWheelAction act = scroll_wheel(k, in, max_px, dim, committed);
    if (act.changed) section_scroll_commit(st, L, axis_h);
    return act;
  }

  // One spring-back animation step for one axis (call at ~60 Hz). Returns
  // true while still animating. Yields without re-committing if something
  // else moved the axis since the last commit (scrollbar drag, API) - the
  // SECTION twin of grid_scroll_bounce_step.
  inline bool section_scroll_bounce_step(SectionScrollState& st,
                                           const SectionLayout& L, bool axis_h)
  {
    ScrollKinetics& k = axis_h ? st.kin_h : st.kin_v;
    int committed = axis_h ? st.scroll_x : st.scroll_y;
    if (committed != k.last_commit_px) {
      // External mutation - drop the overshoot claim so the paint clamp
      // regains authority over this axis.
      if (axis_h) st.kinetic_over_h = false; else st.kinetic_over_v = false;
      return false;
    }
    double max_px = section_scroll_max_px(st, L, axis_h);
    double dim    = axis_h ? (double)L.body_w : (double)L.body_h;
    bool animating = scroll_bounce_step(k, max_px, dim, committed);
    section_scroll_commit(st, L, axis_h);
    return animating;
  }

} // namespace neui_detail
