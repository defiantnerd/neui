#pragma once

#include <neui/neui.h>

// Shared painter forwarder. Both static-lib hosts (xpl and win32 native)
// include this header to build their WIDGET_PAINT event payload. The
// painter exposes only the draw-safe subset of neui_render_backend_t to
// clients (no begin_frame / create_context / raw create_bitmap), and
// resolves neui_asset_t handles through a host-supplied thunk that knows
// how to walk from `host_token` to the session's asset manager.
//
// Per-call overhead vs. calling the backend directly: one extra
// indirection. The painter struct itself is stack-allocated by the host
// inside the paint dispatch site; it lives only for the duration of the
// WIDGET_PAINT callback.
//
// ODR-safe: the forwarders and the static api table are `inline` so the
// linker collapses cross-TU duplicates to one definition.

namespace neui_detail {

  // Thunk that resolves a neui_asset_t against a host-specific manager
  // and draws it to (x, y, w, h) in the current logical coord space.
  // Hosts install one of these per paint via the painter struct below.
  using draw_asset_thunk_t = void (NEUI_ABI *)(
      void* host_token,
      neui_render_backend_t* backend,
      neui_render_ctx_t ctx,
      neui_asset_t asset,
      float x, float y, float w, float h);

} // namespace neui_detail

// Concrete layout of the opaque neui_painter handed to clients via
// neui_event_paint_t::p. Defined here (not in painter.h) so the public
// header keeps the type opaque while the host can construct one on the
// stack. Clients never dereference this; they only pass it back through
// the neui_painter_api_t calls.
struct neui_painter {
  neui_render_backend_t*           backend;
  neui_render_ctx_t                ctx;
  void*                            host_token;       // session / asset mgr ptr (host-defined)
  neui_detail::draw_asset_thunk_t  draw_asset_thunk; // resolves neui_asset_t -> backend->draw_bitmap
};

namespace neui_detail {

  // --- Forwarders --------------------------------------------------------
  // Every entry just calls through to the active backend with ctx applied,
  // except draw_asset which hops through the host's thunk.

  inline float painter_get_scale_factor(neui_painter_t* p)
  {
    return (p && p->backend && p->backend->get_scale_factor)
      ? p->backend->get_scale_factor(p->ctx) : 1.0f;
  }
  inline float painter_measure_text(neui_painter_t* p,
                                     const char* utf8, int text_len,
                                     float font_size)
  {
    return (p && p->backend && p->backend->measure_text)
      ? p->backend->measure_text(p->ctx, utf8, text_len, font_size) : 0.0f;
  }
  inline void painter_fill_rect(neui_painter_t* p,
                                 float x, float y, float w, float h,
                                 uint32_t argb)
  {
    if (p && p->backend && p->backend->fill_rect)
      p->backend->fill_rect(p->ctx, x, y, w, h, argb);
  }
  inline void painter_draw_rect(neui_painter_t* p,
                                 float x, float y, float w, float h,
                                 float stroke_w, uint32_t argb)
  {
    if (p && p->backend && p->backend->draw_rect)
      p->backend->draw_rect(p->ctx, x, y, w, h, stroke_w, argb);
  }
  inline void painter_draw_text(neui_painter_t* p,
                                 float x, float y, float w, float h,
                                 const char* utf8,
                                 float font_size, uint32_t argb)
  {
    if (p && p->backend && p->backend->draw_text)
      p->backend->draw_text(p->ctx, x, y, w, h, utf8, font_size, argb);
  }

  inline void painter_begin_path (neui_painter_t* p)
  { if (p && p->backend && p->backend->begin_path)  p->backend->begin_path(p->ctx); }
  inline void painter_move_to    (neui_painter_t* p, float x, float y)
  { if (p && p->backend && p->backend->move_to)     p->backend->move_to(p->ctx, x, y); }
  inline void painter_line_to    (neui_painter_t* p, float x, float y)
  { if (p && p->backend && p->backend->line_to)     p->backend->line_to(p->ctx, x, y); }
  inline void painter_arc        (neui_painter_t* p, float cx, float cy,
                                   float r, float a_start, float a_end)
  { if (p && p->backend && p->backend->arc)         p->backend->arc(p->ctx, cx, cy, r, a_start, a_end); }
  inline void painter_close_path (neui_painter_t* p)
  { if (p && p->backend && p->backend->close_path)  p->backend->close_path(p->ctx); }
  inline void painter_fill_path  (neui_painter_t* p, uint32_t argb)
  { if (p && p->backend && p->backend->fill_path)   p->backend->fill_path(p->ctx, argb); }
  inline void painter_stroke_path(neui_painter_t* p, float stroke_w, uint32_t argb)
  { if (p && p->backend && p->backend->stroke_path) p->backend->stroke_path(p->ctx, stroke_w, argb); }

  inline void painter_push_clip(neui_painter_t* p,
                                 float x, float y, float w, float h)
  { if (p && p->backend && p->backend->push_clip)      p->backend->push_clip(p->ctx, x, y, w, h); }
  inline void painter_pop_clip(neui_painter_t* p)
  { if (p && p->backend && p->backend->pop_clip)       p->backend->pop_clip(p->ctx); }
  inline void painter_push_transform(neui_painter_t* p)
  { if (p && p->backend && p->backend->push_transform) p->backend->push_transform(p->ctx); }
  inline void painter_pop_transform(neui_painter_t* p)
  { if (p && p->backend && p->backend->pop_transform)  p->backend->pop_transform(p->ctx); }
  inline void painter_translate(neui_painter_t* p, float dx, float dy)
  { if (p && p->backend && p->backend->translate)      p->backend->translate(p->ctx, dx, dy); }
  inline void painter_rotate(neui_painter_t* p, float radians)
  { if (p && p->backend && p->backend->rotate)         p->backend->rotate(p->ctx, radians); }
  inline void painter_scale(neui_painter_t* p, float sx, float sy)
  { if (p && p->backend && p->backend->scale)          p->backend->scale(p->ctx, sx, sy); }

  inline void painter_draw_asset(neui_painter_t* p, neui_asset_t asset,
                                  float x, float y, float w, float h)
  {
    if (!p || !p->draw_asset_thunk) return;
    p->draw_asset_thunk(p->host_token, p->backend, p->ctx, asset, x, y, w, h);
  }

  inline void painter_push_alpha(neui_painter_t* p, float factor)
  { if (p && p->backend && p->backend->push_alpha) p->backend->push_alpha(p->ctx, factor); }
  inline void painter_pop_alpha(neui_painter_t* p)
  { if (p && p->backend && p->backend->pop_alpha)  p->backend->pop_alpha(p->ctx); }

  inline void painter_push_font(neui_painter_t* p, const char* family_utf8, int weight)
  { if (p && p->backend && p->backend->push_font) p->backend->push_font(p->ctx, family_utf8, weight); }
  inline void painter_pop_font(neui_painter_t* p)
  { if (p && p->backend && p->backend->pop_font)  p->backend->pop_font(p->ctx); }

  // The static api table. Each host extern-references this from its
  // paint dispatch and stamps it into neui_event_paint_t::painter_api.
  // `inline` storage makes this ODR-safe across the two TUs that include
  // this header.
  inline neui_painter_api_t k_painter_api = {
    NEUI_VERSION,
    painter_get_scale_factor,
    painter_measure_text,
    painter_fill_rect,
    painter_draw_rect,
    painter_draw_text,
    painter_begin_path,
    painter_move_to,
    painter_line_to,
    painter_arc,
    painter_close_path,
    painter_fill_path,
    painter_stroke_path,
    painter_push_clip,
    painter_pop_clip,
    painter_push_transform,
    painter_pop_transform,
    painter_translate,
    painter_rotate,
    painter_scale,
    painter_draw_asset,
    painter_push_alpha,
    painter_pop_alpha,
    painter_push_font,
    painter_pop_font,
  };

} // namespace neui_detail
