#pragma once

#include <neui/d/attrs.h>
#include <neui/d/renderer.h>
#include <string>

#include "attrs.h"
#include "painter.h"

// Shared helper for reading the per-widget font attrs (family / size /
// weight) and pushing them onto a renderer or painter's font stack
// around a draw_text / measure_text call. Designed for both the xpl
// host and the win32 host's painted-widget seam.
//
// Usage pattern at every text-drawing site:
//
//   auto ef = neui_detail::read_widget_font(wd.attrs.get(), HARDCODED_DEFAULT);
//   neui_detail::push_widget_font(backend, ctx, ef);   // or _painter variant
//   backend->draw_text(ctx, x, y, w, h, text, ef.size, color);
//   neui_detail::pop_widget_font (backend, ctx, ef);
//
// When the widget has none of the font attrs set, family is empty and
// weight is 0 - push_widget_font / pop_widget_font skip the call so
// widgets without font attrs pay zero cost beyond the attr read.

namespace neui_detail
{
  struct EffectiveFont
  {
    std::string family;   // empty = host default (Segoe UI on Win32/D2D)
    float       size = 0.0f;
    int         weight = 0;   // 0 = Normal (CSS 400)
  };

  // True when none of the font attrs were set on the widget - the host
  // can skip push_font / pop_font entirely.
  inline bool font_is_default(const EffectiveFont& f)
  {
    return f.family.empty() && f.weight == 0;
  }

  // Read NEUI_ATTR_FONT_FAMILY / _SIZE / _WEIGHT from the bag. `default_size`
  // (the widget's existing hardcoded font size) is returned in `.size` when
  // NEUI_ATTR_FONT_SIZE is unset or non-positive. Strict typing per attr -
  // calling set_int on a documented-FLOAT key trips assert_attr_kind in
  // debug, so the read here doesn't need to coerce across kinds.
  inline EffectiveFont read_widget_font(const AttrBag* bag, float default_size)
  {
    EffectiveFont f;
    f.size = default_size;
    if (!bag) return f;

    if (const char* fam = bag->get_string(NEUI_ATTR_FONT_FAMILY))
      f.family = fam;

    if (bag->has(NEUI_ATTR_FONT_SIZE)) {
      float s = bag->get_float(NEUI_ATTR_FONT_SIZE, default_size);
      if (s > 0.0f) f.size = s;
    }

    f.weight = bag->get_int(NEUI_ATTR_FONT_WEIGHT, 0);
    return f;
  }

  // Backend-side push / pop. No-op when the effective font matches the
  // host default so widgets without font attrs don't allocate stack entries.
  inline void push_widget_font(neui_render_backend_t* backend,
                                neui_render_ctx_t      ctx,
                                const EffectiveFont&   f)
  {
    if (font_is_default(f)) return;
    if (!backend || !backend->push_font) return;
    backend->push_font(ctx,
                        f.family.empty() ? nullptr : f.family.c_str(),
                        f.weight);
  }

  inline void pop_widget_font(neui_render_backend_t* backend,
                               neui_render_ctx_t      ctx,
                               const EffectiveFont&   f)
  {
    if (font_is_default(f)) return;
    if (!backend || !backend->pop_font) return;
    backend->pop_font(ctx);
  }

  // Painter-side push / pop, for CUSTOMDRAW / compound paint paths that
  // hold a neui_painter_t* rather than a raw backend pointer.
  inline void push_widget_font_painter(neui_painter_t*       p,
                                        const EffectiveFont&  f)
  {
    if (font_is_default(f)) return;
    k_painter_api.push_font(p,
                              f.family.empty() ? nullptr : f.family.c_str(),
                              f.weight);
  }

  inline void pop_widget_font_painter(neui_painter_t*       p,
                                       const EffectiveFont&  f)
  {
    if (font_is_default(f)) return;
    k_painter_api.pop_font(p);
  }

} // namespace neui_detail
