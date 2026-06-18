#pragma once

#include <neui/d/metrics.h>
#include <cstddef>

#include "widget_font.h"          // painted_ui_scale() / scaled_painted_metric()
#include "scrollbar.h"            // SCROLLBAR_W
#include "grid_model.h"           // GRID default row / header heights

// Shared, host-neutral implementation of NEUI_API_METRICS. Every host's
// get_interface returns the SAME vtable (neui_detail::k_metrics_api), so the
// desktop hosts get sensible metrics for free without per-platform tuning:
// painted_ui_scale() is 1.0 on the desktop hosts (so the metrics equal the
// framework's default control / row / font sizes) and the Dynamic-Type-derived
// scale on iOS (so they grow with the user's text-size setting) - all from the
// same code path.
//
// Two methods need a real platform query - text measurement (UIFont on iOS)
// and the frame's safe-area insets (the view's safeAreaInsets on iOS). The
// desktop hosts have no such surface, so the shared vtable carries a desktop
// DEFAULT for each (a font-metric-based width estimate, and zero insets) and
// exposes a function-pointer seam the iOS hosts overwrite at registration with
// the real implementation. Desktop hosts touch nothing - they just return the
// shared vtable.

namespace neui_detail
{
  // ---------------------------------------------------------------------------
  // Base default constants (logical px at scale 1.0). These are the desktop
  // defaults; every size metric is `base * painted_ui_scale()` rounded, so on
  // iOS they auto-scale and on the desktop they are byte-for-byte these values.

  inline constexpr int   METRIC_BASE_CONTROL_HEIGHT  = 24;   // standard control height
  inline constexpr int   METRIC_BASE_CHECKBOX_SIZE   = 16;   // checkbox edge length
  inline constexpr int   METRIC_BASE_MARGIN          = 8;    // outer margin
  inline constexpr int   METRIC_BASE_SPACING         = 6;    // gap between controls
  inline constexpr float METRIC_BASE_BODY_FONT       = 12.0f;// painted default text size
  inline constexpr float METRIC_BASE_LABEL_FONT      = 12.0f;// painted default label size
  // Toggle-switch default size. iOS overrides these with the UISwitch size
  // (51x31); a small desktop default keeps the metric sensible elsewhere.
  inline constexpr int   METRIC_BASE_SWITCH_WIDTH    = 36;
  inline constexpr int   METRIC_BASE_SWITCH_HEIGHT   = 20;

  // ---------------------------------------------------------------------------
  // Per-host seams. Default to the desktop implementations below; the iOS hosts
  // replace them at register_host() with UIFont-backed measurement + the
  // frame's safeAreaInsets. inline so every TU shares the one definition.

  // Desktop default text-width estimate: average glyph advance ~= 0.5 * size.
  // Best-effort - good enough to size a button to its label, not pixel-exact.
  // (The iOS override measures via -sizeWithAttributes:.)
  inline int metrics_measure_text_default(neui_session_t /*session*/,
                                          const char* text, const char* /*family*/,
                                          float size_px, int /*weight*/)
  {
    if (!text || !*text) return 0;
    if (size_px <= 0.0f) size_px = METRIC_BASE_BODY_FONT * painted_ui_scale();
    // Count UTF-8 codepoints (lead bytes), not raw bytes, so multibyte text
    // is not over-measured.
    std::size_t glyphs = 0;
    for (const unsigned char* p = (const unsigned char*)text; *p; ++p)
      if ((*p & 0xC0) != 0x80) ++glyphs;
    return (int)(glyphs * size_px * 0.5f + 0.5f);
  }

  // Desktop default safe-area: no insets (desktop chrome lives outside the
  // client area). The iOS override returns the frame view's safeAreaInsets.
  inline void metrics_safe_area_default(neui_session_t /*session*/,
                                        neui_widget_t /*frame*/,
                                        int* left, int* top, int* right, int* bottom)
  {
    if (left)   *left   = 0;
    if (top)    *top    = 0;
    if (right)  *right  = 0;
    if (bottom) *bottom = 0;
  }

  using metrics_measure_fn   = int  (*)(neui_session_t, const char*, const char*, float, int);
  using metrics_safe_area_fn = void (*)(neui_session_t, neui_widget_t, int*, int*, int*, int*);

  inline metrics_measure_fn&   metrics_measure_seam()
  {
    static metrics_measure_fn fn = &metrics_measure_text_default;
    return fn;
  }
  inline metrics_safe_area_fn& metrics_safe_area_seam()
  {
    static metrics_safe_area_fn fn = &metrics_safe_area_default;
    return fn;
  }

  // ---------------------------------------------------------------------------
  // The shared vtable methods.

  inline float metrics_ui_scale(neui_session_t /*session*/)
  {
    return painted_ui_scale();
  }

  inline int metrics_metric(neui_session_t /*session*/, neui_metric_t m)
  {
    const float scale = painted_ui_scale();
    switch (m) {
      case NEUI_METRIC_CONTROL_HEIGHT:
        return scaled_painted_metric(METRIC_BASE_CONTROL_HEIGHT);
      case NEUI_METRIC_ROW_HEIGHT:
        // The GRID body row default (22) - the canonical "list row" height.
        return scaled_painted_metric(22);
      case NEUI_METRIC_BODY_FONT_SIZE:
        return (int)(METRIC_BASE_BODY_FONT * scale + 0.5f);
      case NEUI_METRIC_LABEL_FONT_SIZE:
        return (int)(METRIC_BASE_LABEL_FONT * scale + 0.5f);
      case NEUI_METRIC_MARGIN:
        return scaled_painted_metric(METRIC_BASE_MARGIN);
      case NEUI_METRIC_SPACING:
        return scaled_painted_metric(METRIC_BASE_SPACING);
      case NEUI_METRIC_CHECKBOX_SIZE:
        return scaled_painted_metric(METRIC_BASE_CHECKBOX_SIZE);
      case NEUI_METRIC_SWITCH_WIDTH:
        return scaled_painted_metric(METRIC_BASE_SWITCH_WIDTH);
      case NEUI_METRIC_SWITCH_HEIGHT:
        return scaled_painted_metric(METRIC_BASE_SWITCH_HEIGHT);
      case NEUI_METRIC_SCROLLBAR_THICKNESS:
        return scaled_painted_metric(SCROLLBAR_W);
    }
    return 0;
  }

  inline int metrics_measure_text(neui_session_t session, const char* text,
                                  const char* family, float size_px, int weight)
  {
    return metrics_measure_seam()(session, text, family, size_px, weight);
  }

  inline void metrics_safe_area(neui_session_t session, neui_widget_t frame,
                                int* left, int* top, int* right, int* bottom)
  {
    metrics_safe_area_seam()(session, frame, left, top, right, bottom);
  }

  // The shared vtable. One definition shared by every host (header-only inline).
  inline neui_metrics_api_t k_metrics_api = {
    NEUI_VERSION,
    metrics_ui_scale,
    metrics_metric,
    metrics_measure_text,
    metrics_safe_area,
  };

} // namespace neui_detail
