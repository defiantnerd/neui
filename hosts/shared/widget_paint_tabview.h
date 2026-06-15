#pragma once

#include <neui/d/renderer.h>
#include <cstring>

#include "widget_tabview.h"
#include "widget_font.h"
#include "theme_palette.h"

// Shared TABVIEW paint. Draws, in order:
//   1. whole-area background (view_bg, optional)
//   2. content-body fill (body_bg)
//   3. the strip area beside the chips (strip_bg, optional - else spared)
//   4. each chip: a fill (rounded on the two OUTER corners by chip_radius)
//      plus a U-shaped outline (the 3 sides away from the body) in sep_color
//   5. a baseline separator along the strip<->body boundary in sep_color,
//      SKIPPING the active chip's span - so the selected tab reads as part of
//      the content while the others are separated by a line
//   6. the content-box border far sides (content_border, optional)
//
// Together, steps 4-6 trace the content box + around the chips (the tab
// outline). There is intentionally NO full-widget rectangle, so the strip
// area beside the chips stays unbordered when it is transparent.
//
// (ox, oy) is the widget's top-left in the active drawing space (xpl passes
// the parent-local x/y; native hosts draw widget-local and pass 0,0). Colour
// 0 means "unset" for strip_bg / content_border and the per-chip override
// arrays.

namespace neui_detail
{
  // Build a chip outline path. The two corners on the OUTER edge (away from
  // the content body) are rounded by `r`; the body-edge corners stay square.
  // Arc sweeps are clockwise (Y-down), matching build_rounded_rect_path. The
  // body edge is always the LAST/closing segment, so passing close=false
  // yields the U of the 3 outer sides (for the separation outline) and
  // close=true yields the full closed shape (for the fill).
  inline void tab_chip_path(neui_render_backend_t* b, neui_render_ctx_t ctx,
                            float x, float y, float w, float h,
                            TabEdge edge, float r, bool close)
  {
    const float PI = 3.14159265358979323846f;
    float mr = (w < h ? w : h) * 0.5f;
    if (r > mr) r = mr;
    if (r < 0.0f) r = 0.0f;
    b->begin_path(ctx);

    if (r <= 0.0f) {
      switch (edge) {
        case TabEdge::Top:    b->move_to(ctx,x,y+h);   b->line_to(ctx,x,y);     b->line_to(ctx,x+w,y);     b->line_to(ctx,x+w,y+h); break;
        case TabEdge::Bottom: b->move_to(ctx,x+w,y);   b->line_to(ctx,x+w,y+h); b->line_to(ctx,x,y+h);     b->line_to(ctx,x,y);     break;
        case TabEdge::Left:   b->move_to(ctx,x+w,y+h); b->line_to(ctx,x,y+h);   b->line_to(ctx,x,y);       b->line_to(ctx,x+w,y);   break;
        case TabEdge::Right:  b->move_to(ctx,x,y);     b->line_to(ctx,x+w,y);   b->line_to(ctx,x+w,y+h);   b->line_to(ctx,x,y+h);   break;
        default:              b->move_to(ctx,x,y);     b->line_to(ctx,x+w,y);   b->line_to(ctx,x+w,y+h);   b->line_to(ctx,x,y+h);   break;
      }
      if (close) b->close_path(ctx);
      return;
    }

    switch (edge) {
      case TabEdge::Top:  // outer = top; body edge = bottom (closing)
        b->move_to(ctx, x, y + h);
        b->line_to(ctx, x, y + r);
        b->arc(ctx, x + r,     y + r, r, PI,        1.5f * PI);
        b->line_to(ctx, x + w - r, y);
        b->arc(ctx, x + w - r, y + r, r, 1.5f * PI, 2.0f * PI);
        b->line_to(ctx, x + w, y + h);
        break;
      case TabEdge::Bottom:  // outer = bottom; body edge = top (closing)
        b->move_to(ctx, x + w, y);
        b->line_to(ctx, x + w, y + h - r);
        b->arc(ctx, x + w - r, y + h - r, r, 0.0f,      0.5f * PI);
        b->line_to(ctx, x + r, y + h);
        b->arc(ctx, x + r,     y + h - r, r, 0.5f * PI, PI);
        b->line_to(ctx, x, y);
        break;
      case TabEdge::Left:  // outer = left; body edge = right (closing)
        b->move_to(ctx, x + w, y + h);
        b->line_to(ctx, x + r, y + h);
        b->arc(ctx, x + r, y + h - r, r, 0.5f * PI, PI);
        b->line_to(ctx, x, y + r);
        b->arc(ctx, x + r, y + r, r, PI, 1.5f * PI);
        b->line_to(ctx, x + w, y);
        break;
      case TabEdge::Right:  // outer = right; body edge = left (closing)
        b->move_to(ctx, x, y);
        b->line_to(ctx, x + w - r, y);
        b->arc(ctx, x + w - r, y + r, r, 1.5f * PI, 2.0f * PI);
        b->line_to(ctx, x + w, y + h - r);
        b->arc(ctx, x + w - r, y + h - r, r, 0.0f, 0.5f * PI);
        b->line_to(ctx, x, y + h);
        break;
      default:
        b->move_to(ctx, x, y); b->line_to(ctx, x + w, y);
        b->line_to(ctx, x + w, y + h); b->line_to(ctx, x, y + h);
        break;
    }
    if (close) b->close_path(ctx);
  }

  // Draw the strip<->body boundary separator, skipping the active chip span
  // (in absolute coords). Two line segments (before + after the active chip).
  inline void tab_draw_baseline(neui_render_backend_t* b, neui_render_ctx_t ctx,
                                float ox, float oy, const TabViewLayout& L,
                                TabEdge edge, const TabChip* active,
                                uint32_t color, float wdt)
  {
    if (!b->begin_path || !b->move_to || !b->line_to || !b->stroke_path) return;
    if (wdt <= 0.0f) wdt = 1.0f;
    auto seg = [&](float x0, float y0, float x1, float y1) {
      if (x1 <= x0 && y1 <= y0) return;
      b->begin_path(ctx);
      b->move_to(ctx, x0, y0);
      b->line_to(ctx, x1, y1);
      b->stroke_path(ctx, wdt, color);
    };
    const bool horiz = (edge == TabEdge::Top || edge == TabEdge::Bottom);
    if (horiz) {
      float by = oy + L.body_y + (edge == TabEdge::Top ? 0.0f : L.body_h);
      float x0 = ox + L.strip_x, x1 = ox + L.strip_x + L.strip_w;
      if (active) {
        float ax0 = ox + active->x, ax1 = ox + active->x + active->w;
        seg(x0, by, ax0, by);
        seg(ax1, by, x1, by);
      } else {
        seg(x0, by, x1, by);
      }
    } else {
      float bx = ox + L.body_x + (edge == TabEdge::Left ? 0.0f : L.body_w);
      float y0 = oy + L.strip_y, y1 = oy + L.strip_y + L.strip_h;
      if (active) {
        float ay0 = oy + active->y, ay1 = oy + active->y + active->h;
        seg(bx, y0, bx, ay0);
        seg(bx, ay1, bx, y1);
      } else {
        seg(bx, y0, bx, y1);
      }
    }
  }

  // Stroke the content body's three sides that are NOT the strip edge
  // (the strip edge is handled by the baseline). Used for the optional
  // content-box border (NEUI_ATTR_TAB_BORDER_COLOR).
  inline void tab_stroke_content_far_sides(neui_render_backend_t* b, neui_render_ctx_t ctx,
                                           float ox, float oy, const TabViewLayout& L,
                                           TabEdge edge, uint32_t color, float wdt)
  {
    if (!b->begin_path || !b->move_to || !b->line_to || !b->stroke_path) return;
    if (wdt <= 0.0f) wdt = 1.0f;
    const float x0 = ox + L.body_x, y0 = oy + L.body_y;
    const float x1 = x0 + L.body_w, y1 = y0 + L.body_h;
    b->begin_path(ctx);
    switch (edge) {
      case TabEdge::Top:    // strip edge = top: stroke left, bottom, right
        b->move_to(ctx, x0, y0); b->line_to(ctx, x0, y1);
        b->line_to(ctx, x1, y1); b->line_to(ctx, x1, y0); break;
      case TabEdge::Bottom: // strip edge = bottom: stroke left, top, right
        b->move_to(ctx, x0, y1); b->line_to(ctx, x0, y0);
        b->line_to(ctx, x1, y0); b->line_to(ctx, x1, y1); break;
      case TabEdge::Left:   // strip edge = left: stroke top, right, bottom
        b->move_to(ctx, x0, y0); b->line_to(ctx, x1, y0);
        b->line_to(ctx, x1, y1); b->line_to(ctx, x0, y1); break;
      case TabEdge::Right:  // strip edge = right: stroke top, left, bottom
        b->move_to(ctx, x1, y0); b->line_to(ctx, x0, y0);
        b->line_to(ctx, x0, y1); b->line_to(ctx, x1, y1); break;
      default: // None: full rectangle
        b->move_to(ctx, x0, y0); b->line_to(ctx, x1, y0);
        b->line_to(ctx, x1, y1); b->line_to(ctx, x0, y1); b->close_path(ctx); break;
    }
    b->stroke_path(ctx, wdt, color);
  }

  inline void paint_tabview(neui_render_backend_t* backend, neui_render_ctx_t ctx,
                            float ox, float oy, float fw, float fh,
                            const TabViewLayout& L, TabEdge edge,
                            const TabChip* chips, int count, int selected, int hover,
                            const char* const* labels,
                            const uint32_t* chip_bg, const uint32_t* chip_text,
                            uint32_t body_bg,
                            uint32_t default_text, uint32_t inactive_chip_bg,
                            uint32_t sep_color, float sep_w,
                            uint32_t strip_bg, uint32_t content_border,
                            float chip_radius,
                            const AttrBag* bag)
  {
    if (!backend || !ctx || !backend->fill_rect) return;
    const bool can_path = backend->begin_path && backend->move_to &&
                          backend->line_to && backend->fill_path && backend->stroke_path;

    // 1. content body fill. (No whole-rect fill: the strip area beside the
    //    chips stays TRANSPARENT unless strip_bg is set, so tabs can float on
    //    the parent background.)
    if (L.body_w > 0 && L.body_h > 0)
      backend->fill_rect(ctx, ox + L.body_x, oy + L.body_y, L.body_w, L.body_h, body_bg);

    // 2. strip area beside the chips - only when explicitly filled.
    if (strip_bg != 0 && L.strip_w > 0 && L.strip_h > 0)
      backend->fill_rect(ctx, ox + L.strip_x, oy + L.strip_y, L.strip_w, L.strip_h, strip_bg);

    // 4. chips: fill (rounded outer corners) + U outline in sep_color.
    const bool can_text = backend->draw_text && backend->measure_text;
    EffectiveFont ef = read_widget_font(bag, TAB_CHIP_FONT);
    if (can_text) push_widget_font(backend, ctx, ef);

    for (int i = 0; i < count; ++i) {
      const TabChip& c = chips[i];
      const bool active = (i == selected);

      uint32_t fill;
      if (chip_bg && chip_bg[i] != 0)      fill = chip_bg[i];
      else if (active)                     fill = body_bg;
      else                                 fill = inactive_chip_bg;
      if (!active && i == hover)           fill = shade(fill, +16);

      if (can_path && chip_radius > 0.0f) {
        tab_chip_path(backend, ctx, ox + c.x, oy + c.y, c.w, c.h, edge, chip_radius, true);
        backend->fill_path(ctx, fill);
        if (sep_color != 0) {
          tab_chip_path(backend, ctx, ox + c.x, oy + c.y, c.w, c.h, edge, chip_radius, false);
          backend->stroke_path(ctx, sep_w, sep_color);
        }
      } else {
        backend->fill_rect(ctx, ox + c.x, oy + c.y, c.w, c.h, fill);
        if (sep_color != 0 && can_path) {
          tab_chip_path(backend, ctx, ox + c.x, oy + c.y, c.w, c.h, edge, 0.0f, false);
          backend->stroke_path(ctx, sep_w, sep_color);
        }
      }

      if (can_text && labels && labels[i] && *labels[i] && c.text_w > 0) {
        uint32_t tc = (chip_text && chip_text[i] != 0) ? chip_text[i] : default_text;
        backend->draw_text(ctx, ox + c.text_x, oy + c.y, c.text_w, c.h,
                           labels[i], ef.size, tc);
      }
    }
    if (can_text) pop_widget_font(backend, ctx, ef);

    // 5. baseline separator (skips the active chip span so it connects).
    if (sep_color != 0 && edge != TabEdge::None) {
      const TabChip* active_chip =
        (selected >= 0 && selected < count) ? &chips[selected] : nullptr;
      tab_draw_baseline(backend, ctx, ox, oy, L, edge, active_chip, sep_color, sep_w);
    }

    // 6. content-box border (far sides) - optional. Together with the chip
    //    outlines + baseline this traces the content box + around the chips
    //    (the tab outline). There is intentionally NO full-widget rectangle:
    //    the strip area beside the chips stays unbordered when transparent.
    if (content_border != 0)
      tab_stroke_content_far_sides(backend, ctx, ox, oy, L, edge, content_border,
                                   sep_w > 0 ? sep_w : 1.0f);
  }

} // namespace neui_detail
