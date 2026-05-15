#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <cstdint>

#include "../theme_palette.h"

// Lazy HBRUSH cache keyed by palette version.
//
// WM_CTLCOLOR* handlers fire many times per paint and need an HBRUSH
// matching the palette role. Creating one per call would leak handles or
// require careful per-call lifetime; instead we cache one brush per role
// and rebuild the whole table whenever the palette version changes.
//
// Header-only `inline` storage so both static libs share the same cache.

namespace neui_detail
{
  inline HBRUSH brush_from_argb(uint32_t argb)
  {
    return CreateSolidBrush(RGB((argb >> 16) & 0xFF,
                                (argb >>  8) & 0xFF,
                                (argb      ) & 0xFF));
  }

  inline COLORREF colorref_from_argb(uint32_t argb)
  {
    return RGB((argb >> 16) & 0xFF,
               (argb >>  8) & 0xFF,
               (argb      ) & 0xFF);
  }

  struct ThemeBrushCache {
    uint32_t version = 0;
    HBRUSH brushes[(size_t)ColorRole::count_] = {};
  };

  inline ThemeBrushCache& theme_brush_cache()
  {
    static ThemeBrushCache c;
    return c;
  }

  inline void invalidate_theme_brushes()
  {
    auto& c = theme_brush_cache();
    for (size_t i = 0; i < (size_t)ColorRole::count_; ++i) {
      if (c.brushes[i]) {
        DeleteObject(c.brushes[i]);
        c.brushes[i] = nullptr;
      }
    }
    c.version = 0;
  }

  inline HBRUSH brush_for_role(ColorRole r)
  {
    auto& c = theme_brush_cache();
    const Palette& p = current_palette();
    if (c.version != p.version) {
      invalidate_theme_brushes();
      c.version = p.version;
    }
    HBRUSH& slot = c.brushes[(size_t)r];
    if (!slot) slot = brush_from_argb(color(p, r));
    return slot;
  }

} // namespace neui_detail

#endif // _WIN32
