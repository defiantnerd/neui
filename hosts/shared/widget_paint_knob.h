#pragma once

#include <neui/d/renderer.h>
#include <cmath>

#include "widget_font.h"

// Shared knob paint helper. Used by both the crossplatform host and the win32
// host's painted-widget seam, so the visual is identical regardless of which
// host instantiated the widget.
//
// Renders a circular knob body with an indicator line that sweeps from
// -135° (value 0) to +135° (value 1). 0° points up (12 o'clock); the sweep
// goes clockwise on screen, so value 0.5 points straight up.
//
// All coordinates are logical pixels at 96 DPI. The (x, y, w, h) rect is
// the widget's bounding box; the helper centres the knob inside it.
//
// If the backend lacks the path API (older backend, null backend), degrades
// to a filled square via fill_rect so the widget still renders something.

namespace neui_detail
{
  // Anchor end of the active fill arc. Maps directly to NEUI_ATTR_POLARITY.
  enum KnobPolarity {
    KNOB_POLARITY_MIN    = 0,  // sweep start (7 o'clock) → value
    KNOB_POLARITY_CENTER = 1,  // 12 o'clock → value (bipolar params)
    KNOB_POLARITY_MAX    = 2,  // sweep end (5 o'clock) → value
  };

  // Map the public string value to the polarity enum. Unknown / null input
  // returns KNOB_POLARITY_MIN to match the default behaviour.
  inline KnobPolarity parse_knob_polarity(const char* s)
  {
    if (!s) return KNOB_POLARITY_MIN;
    // Manual case-insensitive compare against the three accepted spellings.
    auto eq = [](const char* a, const char* b) {
      while (*a && *b) {
        char ca = *a, cb = *b;
        if (ca >= 'A' && ca <= 'Z') ca = static_cast<char>(ca + 32);
        if (cb >= 'A' && cb <= 'Z') cb = static_cast<char>(cb + 32);
        if (ca != cb) return false;
        ++a; ++b;
      }
      return *a == 0 && *b == 0;
    };
    if (eq(s, "center") || eq(s, "centre")) return KNOB_POLARITY_CENTER;
    if (eq(s, "max"))                       return KNOB_POLARITY_MAX;
    return KNOB_POLARITY_MIN;
  }

  inline void paint_knob(neui_render_backend_t* backend,
                          neui_render_ctx_t      ctx,
                          float x, float y, float w, float h,
                          float value,           // [0..1]
                          bool  focused,
                          KnobPolarity polarity = KNOB_POLARITY_MIN,
                          int   steps           = 0,    // 0/1 → continuous, no extra ticks
                          const char* value_text = nullptr, // optional overlay
                          const AttrBag* bag    = nullptr)  // for NEUI_ATTR_FONT_*
  {
    if (!backend || !ctx) return;

    // 0 rad = 3-o'clock (D2D's default). We want 0° = 12-o'clock and a
    // 270° sweep starting at 7-o'clock (-135° from 12) and ending at
    // 5-o'clock (+135° from 12). Subtract π/2 to rotate the whole sweep
    // so 12-o'clock is the centre.
    const float HALF_PI    = 1.5707963267948966f;
    const float DEG_135    = 2.356194490192345f;          // 135° in radians
    const float SWEEP_START = -DEG_135 - HALF_PI;          // 7-o'clock
    const float SWEEP_END   =  DEG_135 - HALF_PI;          // 5-o'clock

    if (value < 0.0f) value = 0.0f;
    if (value > 1.0f) value = 1.0f;

    // Reserve a few pixels outside the disc for a sweep-range track and
    // its end ticks; the disc shrinks slightly so they fit inside the
    // widget bounding box without clipping. When a value_text overlay is
    // present, we also reserve a strip at the bottom of the widget for
    // the text and centre the disc in the remaining area, so the overlay
    // never overlaps the disc.
    const float OUTER_PAD  = 7.0f;
    const float TRACK_GAP  = 2.0f;
    const float TICK_LEN   = 5.0f;
    const float TEXT_H     = 18.0f;
    const float TEXT_FONT  = 12.0f;

    const bool  has_text   = (value_text && *value_text);
    const float disc_h     = has_text ? (h - TEXT_H) : h;  // vertical room for disc
    const float disc_size  = (w < disc_h ? w : disc_h);    // square bounding box
    const float cx         = x + w * 0.5f;
    const float cy         = y + disc_size * 0.5f
                              + (has_text
                                  ? (h - TEXT_H - disc_size) * 0.5f
                                  : (h - disc_size) * 0.5f);
    const float r          = disc_size * 0.5f - OUTER_PAD;
    if (r <= 0.0f) return;
    const float track_r    = r + TRACK_GAP;
    const float tick_inner = r + 1.0f;
    const float tick_outer = tick_inner + TICK_LEN;

    const bool has_path = backend->begin_path && backend->arc &&
                          backend->fill_path && backend->stroke_path &&
                          backend->move_to && backend->line_to;

    if (has_path) {
      // Knob body - filled disc + outline. The full circle is split into
      // two semicircle arcs because Direct2D's AddArc draws nothing when
      // the start point equals the end point (which is the case for a
      // 0..2π single arc starting and ending at 3 o'clock).
      const float PI = 3.14159265358979323846f;
      backend->begin_path(ctx);
      backend->arc(ctx, cx, cy, r, 0.0f, PI);          // 3 o'clock → 9 o'clock (bottom half on screen)
      backend->arc(ctx, cx, cy, r, PI,   2.0f * PI);   // 9 o'clock → 3 o'clock (top half on screen)
      backend->close_path(ctx);
      backend->fill_path(ctx, 0xFF404040);
      backend->stroke_path(ctx, 1.0f,
                            focused ? 0xFFC0C0C0 : 0xFF202020);

      // Sweep-range track - faint full arc from sweep start to sweep end,
      // visualising the available range outside the disc.
      backend->begin_path(ctx);
      backend->arc(ctx, cx, cy, track_r, SWEEP_START, SWEEP_END);
      backend->stroke_path(ctx, 1.0f, 0xFF606060);

      // Active arc - anchored by polarity:
      //   MIN     → from sweep start (7 o'clock) to current value
      //   CENTER  → from 12 o'clock (the sweep midpoint) to current value
      //   MAX     → from sweep end   (5 o'clock) to current value
      // The d2d backend picks sweep direction from the sign of (theta - anchor),
      // so a CENTER polarity arc renders to the left or right of 12 o'clock
      // automatically depending on whether value is below or above 0.5.
      const float theta = SWEEP_START + (SWEEP_END - SWEEP_START) * value;
      float anchor;
      switch (polarity) {
        case KNOB_POLARITY_CENTER: anchor = (SWEEP_START + SWEEP_END) * 0.5f; break;
        case KNOB_POLARITY_MAX:    anchor = SWEEP_END;                        break;
        case KNOB_POLARITY_MIN:
        default:                   anchor = SWEEP_START;                      break;
      }
      const float arc_delta = theta - anchor;
      if (arc_delta > 0.001f || arc_delta < -0.001f) {
        backend->begin_path(ctx);
        backend->arc(ctx, cx, cy, track_r, anchor, theta);
        backend->stroke_path(ctx, 2.0f, 0xFFE0E0E0);
      }

      // Tick marks. With no steps configured (steps < 2) we draw just the
      // two endpoint ticks. With steps >= 2 we draw one tick per step,
      // evenly spaced across the sweep - the endpoints are subsumed by
      // i=0 and i=steps-1.
      const int tick_count = (steps >= 2) ? steps : 2;
      for (int i = 0; i < tick_count; ++i) {
        float a;
        if (tick_count == 2) {
          a = (i == 0) ? SWEEP_START : SWEEP_END;
        } else {
          float t = static_cast<float>(i) / static_cast<float>(tick_count - 1);
          a = SWEEP_START + (SWEEP_END - SWEEP_START) * t;
        }
        const float ca = std::cos(a);
        const float sa = std::sin(a);
        backend->begin_path(ctx);
        backend->move_to(ctx, cx + ca * tick_inner, cy + sa * tick_inner);
        backend->line_to(ctx, cx + ca * tick_outer, cy + sa * tick_outer);
        backend->stroke_path(ctx, 1.5f, 0xFFC0C0C0);
      }

      // Indicator line from centre to 85% of disc radius, at value angle.
      const float ex = cx + std::cos(theta) * r * 0.85f;
      const float ey = cy + std::sin(theta) * r * 0.85f;
      backend->begin_path(ctx);
      backend->move_to(ctx, cx, cy);
      backend->line_to(ctx, ex, ey);
      backend->stroke_path(ctx, 2.0f, 0xFFFFFFFF);

      // Value-text overlay below the disc when set. Honour NEUI_ATTR_FONT_*
      // for the overlay - size override changes the rendered glyph height
      // (we don't reshape TEXT_H, so a much larger size will clip into
      // TEXT_H's band; the default 12 fits comfortably).
      if (has_text && backend->draw_text) {
        EffectiveFont ef = read_widget_font(bag, TEXT_FONT);
        push_widget_font(backend, ctx, ef);
        float text_w = w;
        if (backend->measure_text) {
          float mw = backend->measure_text(ctx, value_text, -1, ef.size);
          if (mw > 0.0f && mw < w) text_w = mw;
        }
        float text_x = x + (w - text_w) * 0.5f;
        float text_y = y + h - TEXT_H;
        uint32_t text_color = focused ? 0xFFFFFFFF : 0xFFE0E0E0;
        backend->draw_text(ctx, text_x, text_y, text_w, TEXT_H,
                            value_text, ef.size, text_color);
        pop_widget_font(backend, ctx, ef);
      }
    } else if (backend->fill_rect) {
      backend->fill_rect(ctx, x, y, w, h, 0xFF404040);
      if (backend->draw_rect && focused)
        backend->draw_rect(ctx, x, y, w, h, 1.5f, 0xFFC0C0C0);
    }
  }
} // namespace neui_detail
