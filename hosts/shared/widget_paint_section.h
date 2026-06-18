#pragma once

#include <neui/d/renderer.h>
#include <cstring>

#include "widget_font.h"

// Shared SECTION paint helper. Used by both the crossplatform host and the
// win32 host's painted-widget seam so the visual is identical regardless of
// which host instantiated the widget.
//
// Coordinate convention: SECTION children's (x, y) is relative to the
// BODY's top-left, not the section's top-left. When the chip is hidden
// (align="none" or empty text), the body fills the whole rect, so
// children appear at the section's top edge. When the chip is present,
// the body starts at y == band_h (22 logical px), so children with y=0
// land just below the band.
//
// Layout (when `text` is non-null/non-empty):
//   +-------------------------------------------------+
//   |               [ title chip ]                    |  <- header band
//   | (band's non-chip area is left UNPAINTED so the  |
//   |  caller can make it transparent via clipping /  |
//   |  window region)                                 |
//   +-------------------------------------------------+
//   |                                                 |
//   |                    body                         |  <- fully filled
//   |                                                 |
//   +-------------------------------------------------+
//
// Chip horizontal position selected by `align` ("none"|"left"|"center"|
// "right"; nullptr or unrecognised = "center"). "none" suppresses the
// chip + the header band entirely - the body fills the whole rect (same
// path as an empty text). The body always fills full width below the band.
//
// If `text` is null / empty / `align="none"` (or the backend is missing
// draw_text / measure_text), the whole rect is filled with `bg_argb` and
// no header band is reserved.
//
// All coordinates are logical pixels at 96 DPI. Colours are 0xAARRGGBB.

namespace neui_detail
{
  inline constexpr float SECTION_LABEL_PAD_X = 8.0f;
  inline constexpr float SECTION_LABEL_FONT  = 12.0f;
  inline constexpr float SECTION_HEADER_H    = 22.0f;
  inline constexpr int   SECTION_BG_LIFT     = 24;   // shade delta over frame_bg

  // Chip header band height, scaled by painted_ui_scale() so the band grows
  // to fit the (scaled) chip label on iOS; identity (== SECTION_HEADER_H) on
  // desktop. This is the single source of truth shared by the paint geometry
  // (section_chip_rect) and the children body-offset (section_band_h_for) so
  // they always agree.
  inline float section_header_h()
  {
    return SECTION_HEADER_H * painted_ui_scale();
  }

  struct SectionChip {
    float band_h;   // height of the header band
    float chip_x;   // left edge of the title chip
    float chip_w;   // chip width  (text width + 2 * pad, clamped to fw)
    float text_x;   // left edge of the title text inside the chip
    float text_w;   // text-draw width (clamped non-negative)
  };

  // Compute the title chip geometry for a given section rect and text width.
  // Caller supplies the measured text width (from backend->measure_text);
  // helper handles padding, clamping, and the three alignment cases.
  inline SectionChip section_chip_rect(float fx, float fw, float fh,
                                       float text_width,
                                       const char* align)
  {
    SectionChip c{};
    const float header_h = section_header_h();
    c.band_h = (header_h < fh) ? header_h : fh;
    c.chip_w = text_width + 2.0f * SECTION_LABEL_PAD_X;
    if (c.chip_w > fw) c.chip_w = fw;
    if (align && !std::strcmp(align, "left")) {
      c.chip_x = fx;
    } else if (align && !std::strcmp(align, "right")) {
      c.chip_x = fx + fw - c.chip_w;
    } else {
      // Default and "center": chip centred over the section's painted area.
      c.chip_x = fx + (fw - c.chip_w) * 0.5f;
    }
    c.text_x = c.chip_x + SECTION_LABEL_PAD_X;
    c.text_w = c.chip_w - 2.0f * SECTION_LABEL_PAD_X;
    if (c.text_w < 0.0f) c.text_w = 0.0f;
    return c;
  }

  // Paint the section: body fill + title chip fill + title text. The
  // band's non-chip area is intentionally left UNPAINTED - the caller is
  // responsible for any clipping / window region that makes those pixels
  // transparent (e.g. via SetWindowRgn on the win32 host, or by simply
  // not overwriting the frame's earlier paint in the xpl host).
  // True when `align` requests the no-band path: "none" hides chip + band
  // entirely (body fills the whole rect). Used both by the paint helper
  // and by the win32 window-region builder.
  inline bool section_align_is_none(const char* align)
  {
    return align && !std::strcmp(align, "none");
  }

  inline void paint_section(neui_render_backend_t* backend,
                            neui_render_ctx_t      ctx,
                            float fx, float fy, float fw, float fh,
                            const char* text,
                            uint32_t    bg_argb,
                            const char* align,
                            uint32_t    text_argb,
                            const AttrBag* bag = nullptr)
  {
    if (!backend || !ctx) return;

    // Untitled section / chip explicitly suppressed: one flat fill, no
    // band reserved.
    if (!text || !*text || section_align_is_none(align)
        || !backend->draw_text || !backend->measure_text) {
      if (backend->fill_rect)
        backend->fill_rect(ctx, fx, fy, fw, fh, bg_argb);
      return;
    }

    EffectiveFont ef = read_widget_font(bag, SECTION_LABEL_FONT);
    push_widget_font(backend, ctx, ef);

    float tw = backend->measure_text(ctx, text, -1, ef.size);
    SectionChip c = section_chip_rect(fx, fw, fh, tw, align);

    // Body fills the rect below the band with the section colour.
    if (fh > c.band_h && backend->fill_rect)
      backend->fill_rect(ctx, fx, fy + c.band_h, fw, fh - c.band_h, bg_argb);

    // Title chip - tight bounding box around the text.
    if (backend->fill_rect)
      backend->fill_rect(ctx, c.chip_x, fy, c.chip_w, c.band_h, bg_argb);

    // Title text - vertically centred in the band by the backend's
    // paragraph alignment, horizontally inside the chip's padded interior.
    backend->draw_text(ctx, c.text_x, fy, c.text_w, c.band_h,
                       text, ef.size, text_argb);

    pop_widget_font(backend, ctx, ef);
  }
} // namespace neui_detail
