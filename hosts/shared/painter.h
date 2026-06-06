#pragma once

#include <cstdint>
#include <vector>

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
  //
  // `tint` is an ARGB multiplicative tint applied to the bitmap's pixels.
  // 0xFFFFFFFFu (the public painter API's default) is the passthrough:
  // it short-circuits the tinted-upload path and reuses the standard
  // per-(asset, ctx) bitmap cache, so untinted draws stay byte-for-byte
  // identical to the pre-tint shipping behaviour. Any other value forces
  // a CPU pre-tint + a per-(asset, ctx, tint) cache lookup; see
  // draw_tinted_bitmap_from_entry below.
  using draw_asset_thunk_t = void (NEUI_ABI *)(
      void* host_token,
      neui_render_backend_t* backend,
      neui_render_ctx_t ctx,
      neui_asset_t asset,
      float x, float y, float w, float h,
      uint32_t tint);

  // Tinted-bitmap cache slot stored on the host's AssetEntry. The three
  // hosts each grow a `std::vector<TintedCtxBitmap> tinted_bitmaps;`
  // alongside their host-local per-(asset, ctx) `bitmaps` map. Linear
  // scan; expected size is 1-3 entries per tinted asset.
  struct TintedCtxBitmap
  {
    neui_render_ctx_t ctx        = nullptr;
    uint32_t          tint       = 0xFFFFFFFFu;
    void*             bmp        = nullptr;
    uint32_t          generation = 0;
  };

  // Multiply BGRA8 premultiplied pixels by an ARGB tint. The tint's A
  // channel scales source alpha; R/G/B channels scale the matching
  // pre-multiplied colour channels, so monochrome icons painted as
  // (255, 255, 255, alpha) come out the tint's colour at the same
  // alpha shape. Caller guarantees dst has the same size as src;
  // dst may alias src.
  inline void premultiply_tint(const uint8_t* src, uint8_t* dst,
                                 uint32_t width_px, uint32_t height_px,
                                 uint32_t tint)
  {
    uint32_t ta = (tint >> 24) & 0xffu;
    uint32_t tr = (tint >> 16) & 0xffu;
    uint32_t tg = (tint >>  8) & 0xffu;
    uint32_t tb = (tint >>  0) & 0xffu;
    size_t n = static_cast<size_t>(width_px) * height_px;
    for (size_t i = 0; i < n; ++i) {
      // BGRA byte order: 0 = B, 1 = G, 2 = R, 3 = A.
      dst[i*4 + 0] = static_cast<uint8_t>((src[i*4 + 0] * tb) / 255u);
      dst[i*4 + 1] = static_cast<uint8_t>((src[i*4 + 1] * tg) / 255u);
      dst[i*4 + 2] = static_cast<uint8_t>((src[i*4 + 2] * tr) / 255u);
      dst[i*4 + 3] = static_cast<uint8_t>((src[i*4 + 3] * ta) / 255u);
    }
  }

  // Look up or upload a tinted GPU bitmap for (entry, ctx, tint) and
  // draw it. Entry must expose `width_px`, `height_px`, `scale`,
  // `pixels` (contiguous BGRA8 buffer) and a
  // `std::vector<TintedCtxBitmap> tinted_bitmaps;` cache. Templatised
  // over Entry to share the body across the three host AssetEntry
  // types without forcing a unified type.
  template <typename Entry>
  inline void draw_tinted_bitmap_from_entry(
      neui_render_backend_t* backend,
      neui_render_ctx_t ctx,
      Entry* entry,
      float x, float y, float w, float h,
      uint32_t tint)
  {
    if (!backend || !ctx || !entry) return;
    if (!backend->create_bitmap || !backend->draw_bitmap) return;
    if (entry->pixels.empty()) return;
    if (entry->width_px == 0 || entry->height_px == 0) return;

    const uint32_t gen = backend->get_context_generation
      ? backend->get_context_generation(ctx) : 0u;

    // Linear scan over tinted slots; expected count is small (1-3).
    TintedCtxBitmap* found = nullptr;
    for (auto& slot : entry->tinted_bitmaps) {
      if (slot.ctx == ctx && slot.tint == tint) { found = &slot; break; }
    }

    if (found && found->bmp && found->generation != gen) {
      // Device-loss on the owning ctx - drop the dangling handle, then
      // fall into the re-upload branch below.
      if (backend->destroy_bitmap) backend->destroy_bitmap(ctx, found->bmp);
      found->bmp = nullptr;
    }

    if (!found || !found->bmp) {
      std::vector<uint8_t> tinted(entry->pixels.size());
      premultiply_tint(entry->pixels.data(), tinted.data(),
                         entry->width_px, entry->height_px, tint);
      void* new_bmp = backend->create_bitmap(ctx,
                                              entry->width_px, entry->height_px,
                                              tinted.data(),
                                              entry->scale);
      if (!new_bmp) return;
      if (found) {
        found->bmp        = new_bmp;
        found->generation = gen;
      } else {
        entry->tinted_bitmaps.push_back(
          TintedCtxBitmap{ ctx, tint, new_bmp, gen });
        found = &entry->tinted_bitmaps.back();
      }
    }

    backend->draw_bitmap(ctx, found->bmp,
                           0.0f, 0.0f, 0.0f, 0.0f,   // full bitmap
                           x, y, w, h);
  }

  // Release every tinted_bitmaps slot whose ctx matches. Used by the
  // hosts' release_context paths alongside the untinted bitmaps cache.
  template <typename Entry>
  inline void release_tinted_bitmaps_for_ctx(
      Entry* entry,
      neui_render_ctx_t ctx,
      neui_render_backend_t* backend)
  {
    if (!entry) return;
    auto& v = entry->tinted_bitmaps;
    auto new_end = v.begin();
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (it->ctx == ctx) {
        if (backend && backend->destroy_bitmap && it->bmp)
          backend->destroy_bitmap(ctx, it->bmp);
        continue;
      }
      if (new_end != it) *new_end = *it;
      ++new_end;
    }
    v.erase(new_end, v.end());
  }

  // Release every tinted_bitmaps slot, calling destroy_bitmap on each
  // slot's owning ctx. Used by entry teardown paths (release_slot,
  // manager clear).
  template <typename Entry>
  inline void release_all_tinted_bitmaps(
      Entry* entry,
      neui_render_backend_t* backend)
  {
    if (!entry) return;
    if (backend && backend->destroy_bitmap) {
      for (auto& slot : entry->tinted_bitmaps)
        if (slot.bmp) backend->destroy_bitmap(slot.ctx, slot.bmp);
    }
    entry->tinted_bitmaps.clear();
  }

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
    p->draw_asset_thunk(p->host_token, p->backend, p->ctx, asset,
                          x, y, w, h, 0xFFFFFFFFu);
  }

  // Tinted variant - bypasses the public painter API (which does not
  // expose `tint`) and reaches the thunk directly. Used by the compound
  // asset-layer paint path; clients of NEUI_W_CUSTOMDRAW continue to use
  // k_painter_api.draw_asset.
  inline void painter_draw_asset_tinted(neui_painter_t* p, neui_asset_t asset,
                                          float x, float y, float w, float h,
                                          uint32_t tint)
  {
    if (!p || !p->draw_asset_thunk) return;
    p->draw_asset_thunk(p->host_token, p->backend, p->ctx, asset,
                          x, y, w, h, tint);
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
