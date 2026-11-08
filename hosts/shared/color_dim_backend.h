#pragma once

#include <cstddef>   // offsetof
#include <cstdint>
#include <vector>

#include <neui/d/gradient.h>
#include <neui/d/renderer.h>

#include "disabled_dim.h"

// The colour part of NEUI_ATTR_DISABLED_DIM (disabled_dim.h): a render backend
// that scales R, G and B of everything one paint pass draws on one context. It
// wraps the real backend's vtable; no backend implements anything for it.
//
// Why this, and not an off-screen layer per dimmed widget:
//
//   - Every pixel a widget paints comes from a backend call that carries its
//     colour: a solid ARGB (fill, stroke, text), gradient stops, or a bitmap's
//     multiplicative tint. Scaling a colour channel commutes with source-over
//     compositing and with gradient interpolation, so scaling each call's
//     colour gives the same pixels as scaling the finished widget - with the
//     same code on CoreGraphics, Direct2D, Cairo and Canvas 2D.
//   - Bitmaps (compound asset layers, filmstrips, QR symbols, the CPU image of
//     a shader) already go through draw_bitmap's tint, which every backend
//     implements as a multiply; the dim folds into the tint.
//   - A GPU shader program writes the target's pixels directly, where nothing
//     can scale them, so the wrapper has no GPU shader slots: the host then
//     draws the shader's CPU image, a bitmap, which is tinted.
//   - An off-screen layer would cost a surface, a composite and (on most
//     backends) a read-back or upload per dimmed widget per frame, and would
//     have to reproduce the target's transform, clip and device grid; text
//     drawn off-screen also renders differently from on-screen on Direct2D.
//     Here the cost is a multiply per draw call, and a widget's damage rect is
//     the same dimmed or not, so partial repaint needs nothing new.
//
// The wrapper is a copy of the real vtable with only the colour-carrying slots
// replaced, so everything else - paths, transforms, clips, fonts, measuring,
// bitmap uploads - IS the real backend. The replaced slots find the active
// wrapper through one static (painting is single-threaded, as for the palette
// override in theme_palette.h; a nested wrapper restores the outer one when it
// goes) and scale only draws on the context the wrapper was made for. A draw
// on any other context - an off-screen surface a client paints from its
// WIDGET_PAINT - passes through unscaled, so what it later draws onto the
// target is scaled once, when it lands there.
//
// Opacity is not touched here: the host pushes the dim's A factor on the
// backend's alpha stack, and a bitmap's tint keeps its own alpha byte.

namespace neui_detail
{
  // A slot appended to neui_render_backend_t must be looked at here: if it
  // carries a colour, ColorDimBackend has to scale it, or a disabled widget
  // that draws with it is not dimmed.
  static_assert(offsetof(neui_render_backend_t, draw_shader_program_textured) + sizeof(void*)
                  == sizeof(neui_render_backend_t),
                "neui_render_backend_t grew: check whether ColorDimBackend must scale the new slot");

  // A gradient with every stop's colour scaled; the stops live here.
  class DimmedGradient
  {
  public:
    DimmedGradient(const neui_gradient_t& src, uint32_t dim) : _grad(src)
    {
      neui_gradient_stop_t* out = _stops;
      if (src.stop_count > k_inline_stops) {
        _more.resize(src.stop_count);
        out = _more.data();
      }
      for (uint32_t i = 0; i < src.stop_count; ++i) {
        out[i] = src.stops[i];
        out[i].argb = dim_color(src.stops[i].argb, dim);
      }
      _grad.stops = out;
    }
    DimmedGradient(const DimmedGradient&)            = delete;
    DimmedGradient& operator=(const DimmedGradient&) = delete;

    const neui_gradient_t* get() const { return &_grad; }

  private:
    static constexpr uint32_t k_inline_stops = 16;
    neui_gradient_t                   _grad;
    neui_gradient_stop_t              _stops[k_inline_stops];
    std::vector<neui_gradient_stop_t> _more;
  };

  class ColorDimBackend
  {
  public:
    // Scales R, G and B of every draw on `ctx` by `dim`'s (dim_color). backend()
    // is usable while this object lives.
    ColorDimBackend(const neui_render_backend_t* real, neui_render_ctx_t ctx, uint32_t dim)
      : _table(*real), _real(real), _ctx(ctx), _dim(dim), _prev(active())
    {
      if (real->fill_rect)            _table.fill_rect            = &w_fill_rect;
      if (real->draw_rect)            _table.draw_rect            = &w_draw_rect;
      if (real->draw_text)            _table.draw_text            = &w_draw_text;
      if (real->draw_bitmap)          _table.draw_bitmap          = &w_draw_bitmap;
      if (real->fill_path)            _table.fill_path            = &w_fill_path;
      if (real->stroke_path)          _table.stroke_path          = &w_stroke_path;
      if (real->fill_rect_gradient)   _table.fill_rect_gradient   = &w_fill_rect_gradient;
      if (real->fill_path_gradient)   _table.fill_path_gradient   = &w_fill_path_gradient;
      if (real->stroke_path_styled)   _table.stroke_path_styled   = &w_stroke_path_styled;
      if (real->stroke_path_gradient) _table.stroke_path_gradient = &w_stroke_path_gradient;
      // NULL GPU draw slots mean "shaders on the CPU" (renderer.h): the
      // shader's image is then a bitmap, and draw_bitmap tints it.
      _table.draw_shader_program          = nullptr;
      _table.draw_shader_program_textured = nullptr;
      active() = this;
    }
    ~ColorDimBackend() { active() = _prev; }

    ColorDimBackend(const ColorDimBackend&)            = delete;
    ColorDimBackend& operator=(const ColorDimBackend&) = delete;

    neui_render_backend_t* backend() { return &_table; }

  private:
    neui_render_backend_t        _table;
    const neui_render_backend_t* _real;
    neui_render_ctx_t            _ctx;
    uint32_t                     _dim;
    ColorDimBackend*             _prev;

    static ColorDimBackend*& active()
    {
      static ColorDimBackend* current = nullptr;
      return current;
    }

    // `argb` as the draw on `ctx` should use it.
    uint32_t color_for(neui_render_ctx_t ctx, uint32_t argb) const
    {
      return ctx == _ctx ? dim_color(argb, _dim) : argb;
    }

    static void NEUI_ABI w_fill_rect(neui_render_ctx_t ctx, float x, float y, float w, float h,
                                     uint32_t argb)
    {
      if (const ColorDimBackend* d = active())
        d->_real->fill_rect(ctx, x, y, w, h, d->color_for(ctx, argb));
    }

    static void NEUI_ABI w_draw_rect(neui_render_ctx_t ctx, float x, float y, float w, float h,
                                     float stroke_width, uint32_t argb)
    {
      if (const ColorDimBackend* d = active())
        d->_real->draw_rect(ctx, x, y, w, h, stroke_width, d->color_for(ctx, argb));
    }

    static void NEUI_ABI w_draw_text(neui_render_ctx_t ctx, float x, float y, float w, float h,
                                     const char* text, float font_size, uint32_t argb)
    {
      if (const ColorDimBackend* d = active())
        d->_real->draw_text(ctx, x, y, w, h, text, font_size, d->color_for(ctx, argb));
    }

    // The tint is a multiply already; the dim joins it. Its alpha byte stays
    // (0xFFFFFFFF, "no tint", becomes an opaque tint of the dim's colour).
    static void NEUI_ABI w_draw_bitmap(neui_render_ctx_t ctx, void* bitmap,
                                       float src_x, float src_y, float src_w, float src_h,
                                       float dst_x, float dst_y, float dst_w, float dst_h,
                                       uint32_t tint)
    {
      if (const ColorDimBackend* d = active())
        d->_real->draw_bitmap(ctx, bitmap, src_x, src_y, src_w, src_h,
                              dst_x, dst_y, dst_w, dst_h, d->color_for(ctx, tint));
    }

    static void NEUI_ABI w_fill_path(neui_render_ctx_t ctx, uint32_t argb)
    {
      if (const ColorDimBackend* d = active())
        d->_real->fill_path(ctx, d->color_for(ctx, argb));
    }

    static void NEUI_ABI w_stroke_path(neui_render_ctx_t ctx, float stroke_width, uint32_t argb)
    {
      if (const ColorDimBackend* d = active())
        d->_real->stroke_path(ctx, stroke_width, d->color_for(ctx, argb));
    }

    static void NEUI_ABI w_stroke_path_styled(neui_render_ctx_t ctx, float stroke_width,
                                              uint32_t argb, const neui_stroke_style_t* style)
    {
      if (const ColorDimBackend* d = active())
        d->_real->stroke_path_styled(ctx, stroke_width, d->color_for(ctx, argb), style);
    }

    // Gradients: the stops are copied with their colours scaled. A gradient the
    // backend would not draw (null, no stops) passes through as it is.
    static bool scales_gradient(const ColorDimBackend* d, neui_render_ctx_t ctx,
                                const neui_gradient_t* grad)
    {
      return ctx == d->_ctx && grad && grad->stops && grad->stop_count > 0;
    }

    static void NEUI_ABI w_fill_rect_gradient(neui_render_ctx_t ctx, float x, float y,
                                              float w, float h, const neui_gradient_t* grad)
    {
      const ColorDimBackend* d = active();
      if (!d) return;
      if (!scales_gradient(d, ctx, grad)) {
        d->_real->fill_rect_gradient(ctx, x, y, w, h, grad);
        return;
      }
      DimmedGradient g(*grad, d->_dim);
      d->_real->fill_rect_gradient(ctx, x, y, w, h, g.get());
    }

    static void NEUI_ABI w_fill_path_gradient(neui_render_ctx_t ctx, const neui_gradient_t* grad)
    {
      const ColorDimBackend* d = active();
      if (!d) return;
      if (!scales_gradient(d, ctx, grad)) {
        d->_real->fill_path_gradient(ctx, grad);
        return;
      }
      DimmedGradient g(*grad, d->_dim);
      d->_real->fill_path_gradient(ctx, g.get());
    }

    static void NEUI_ABI w_stroke_path_gradient(neui_render_ctx_t ctx, float stroke_width,
                                                const neui_gradient_t* grad,
                                                const neui_stroke_style_t* style)
    {
      const ColorDimBackend* d = active();
      if (!d) return;
      if (!scales_gradient(d, ctx, grad)) {
        d->_real->stroke_path_gradient(ctx, stroke_width, grad, style);
        return;
      }
      DimmedGradient g(*grad, d->_dim);
      d->_real->stroke_path_gradient(ctx, stroke_width, g.get(), style);
    }
  };

} // namespace neui_detail
