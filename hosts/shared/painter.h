#pragma once

#include <cstdint>
#include <type_traits>
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
  // `tint` is an ARGB multiplicative tint applied to the bitmap's pixels;
  // the thunk passes it straight through to backend->draw_bitmap so each
  // backend can run its native multiplicative-tint primitive (D2D effect
  // on Windows, blend-mode multiply on macOS). 0xFFFFFFFFu is the
  // passthrough sentinel: the backend bypasses tint setup entirely and
  // the draw stays byte-for-byte identical to untinted code paths.
  // Sentinel `frame` value meaning "draw the whole bitmap" rather than a
  // single filmstrip cell. painter_draw_asset / _tinted pass this; the
  // frame-aware painter_draw_asset_frame / _tinted pass a real frame index.
  // A filmstrip-tagged asset drawn with this sentinel shows the entire strip
  // (back-compat: plain draw_asset on a tagged asset draws the whole image).
  inline constexpr uint32_t k_draw_asset_whole = 0xFFFFFFFFu;

  // Thunk that resolves a neui_asset_t against a host-specific manager and
  // draws it. `frame` selects a filmstrip cell, or k_draw_asset_whole for the
  // whole bitmap. Hosts install one of these per paint via the painter struct
  // below.
  using draw_asset_thunk_t = void (NEUI_ABI *)(
      void* host_token,
      neui_render_backend_t* backend,
      neui_render_ctx_t ctx,
      neui_asset_t asset,
      float x, float y, float w, float h,
      uint32_t frame,
      uint32_t tint);

  // Lazy GPU upload for the (entry, ctx) pair with device-loss check.
  // Returns the cached/freshly-uploaded backend bitmap handle, or nullptr
  // if the backend can't create one. If the backend has bumped its per-ctx
  // generation (D2D after D2DERR_RECREATE_TARGET) the cached handle is
  // dangling - it's dropped and re-uploaded against the new target.
  //
  // EntryT is each host's asset-entry type; it needs `bitmaps` (an
  // unordered_map<neui_render_ctx_t, {void* bmp; uint32_t generation;}>),
  // `width_px`, `height_px`, `pixels`, `scale`.
  template <typename EntryT>
  inline void* upload_entry_bitmap(neui_render_backend_t* backend,
                                    neui_render_ctx_t ctx, EntryT* entry)
  {
    using CtxBitmapT =
      typename std::decay<decltype(entry->bitmaps)>::type::mapped_type;
    const uint32_t gen = backend->get_context_generation
      ? backend->get_context_generation(ctx) : 0u;
    auto it = entry->bitmaps.find(ctx);
    if (it != entry->bitmaps.end() && it->second.generation != gen) {
      if (backend->destroy_bitmap && it->second.bmp)
        backend->destroy_bitmap(ctx, it->second.bmp);
      entry->bitmaps.erase(it);
      it = entry->bitmaps.end();
    }
    if (it == entry->bitmaps.end()) {
      if (!backend->create_bitmap) return nullptr;
      void* bmp = backend->create_bitmap(ctx,
                                          entry->width_px, entry->height_px,
                                          entry->pixels.data(),
                                          entry->scale);
      if (!bmp) return nullptr;
      it = entry->bitmaps.emplace(ctx, CtxBitmapT{ bmp, gen }).first;
    }
    return it->second.bmp;
  }

  // Compute the physical-px source rect of frame `frame` within a frame
  // strip laid out as a `cols`-wide row-major grid of `frame_w_px` x
  // `frame_h_px` cells separated by `gutter_px`. `frame` clamps into
  // [0, frame_count) so a value past the end pins to the last frame
  // (mirrors a knob value of 1.0 mapping to the final frame). Single
  // source of truth shared by the draw path and the store's frame_src_rect.
  inline void filmstrip_src_rect(uint32_t frame_count, uint32_t cols,
                                 uint32_t frame_w_px, uint32_t frame_h_px,
                                 uint32_t gutter_px, uint32_t frame,
                                 float& sx, float& sy, float& sw, float& sh)
  {
    const uint32_t fc = frame_count ? frame_count : 1u;
    if (frame >= fc) frame = fc - 1u;
    const uint32_t c   = cols ? cols : 1u;
    const uint32_t col = frame % c;
    const uint32_t row = frame / c;
    sx = static_cast<float>(col * (frame_w_px + gutter_px));
    sy = static_cast<float>(row * (frame_h_px + gutter_px));
    sw = static_cast<float>(frame_w_px);
    sh = static_cast<float>(frame_h_px);
  }

  // Shared body for the per-host draw_asset thunks: lazy GPU upload for the
  // (entry, ctx) pair, then draw the whole bitmap. `tint` routes straight to
  // the backend's draw_bitmap (0xFFFFFFFFu = untinted passthrough). The host
  // thunk keeps the host-specific part (session cast + cross-session
  // validation + slot resolution) and forwards the entry here.
  template <typename EntryT>
  inline void painter_draw_entry_cached(neui_render_backend_t* backend,
                                         neui_render_ctx_t ctx,
                                         EntryT* entry,
                                         float x, float y, float w, float h,
                                         uint32_t tint)
  {
    if (!backend || !ctx || !entry) return;
    void* bmp = upload_entry_bitmap(backend, ctx, entry);
    if (bmp && backend->draw_bitmap)
      backend->draw_bitmap(ctx, bmp,
                            0.0f, 0.0f, 0.0f, 0.0f,    // full bitmap
                            x, y, w, h, tint);
  }

  // Frame-aware sibling of painter_draw_entry_cached: draws frame `frame` of
  // a filmstrip-tagged entry into the dest rect, sampling the cell sub-rect
  // from the single cached upload of the whole strip. If the entry carries no
  // `filmstrip` layout this draws the whole bitmap, so it degrades to
  // painter_draw_entry_cached. EntryT additionally needs a `filmstrip` member
  // convertible to bool exposing { frame_count, cols, frame_w_px, frame_h_px,
  // gutter_px } (neui_detail::FilmstripInfo*, null = plain bitmap).
  template <typename EntryT>
  inline void painter_draw_entry_frame_cached(neui_render_backend_t* backend,
                                               neui_render_ctx_t ctx,
                                               EntryT* entry, uint32_t frame,
                                               float x, float y, float w, float h,
                                               uint32_t tint)
  {
    if (!backend || !ctx || !entry) return;
    void* bmp = upload_entry_bitmap(backend, ctx, entry);
    if (!bmp || !backend->draw_bitmap) return;
    float sx = 0.0f, sy = 0.0f, sw = 0.0f, sh = 0.0f;  // 0,0,0,0 = full bitmap
    if (entry->filmstrip) {
      const auto& fs = *entry->filmstrip;
      filmstrip_src_rect(fs.frame_count, fs.cols, fs.frame_w_px, fs.frame_h_px,
                         fs.gutter_px, frame, sx, sy, sw, sh);
      // filmstrip_src_rect yields PHYSICAL px; draw_bitmap's source rect is in
      // the bitmap's LOGICAL space (every backend re-applies the bitmap scale
      // internally), so divide out the scale here. At scale 1.0 this is a
      // no-op; without it an @2x / HiDPI strip samples the wrong cell.
      const float inv = (entry->scale > 0.0f) ? (1.0f / entry->scale) : 1.0f;
      sx *= inv; sy *= inv; sw *= inv; sh *= inv;
    }
    backend->draw_bitmap(ctx, bmp, sx, sy, sw, sh, x, y, w, h, tint);
  }

  // Sentinel-aware dispatch shared by every host's draw_asset thunk: the
  // k_draw_asset_whole sentinel routes to the whole-bitmap draw, any real
  // frame index to the cell sampler (which itself degrades to a whole-bitmap
  // draw on an untagged entry). Keeps the whole-vs-cell rule in one place so a
  // future change can't silently diverge across hosts.
  template <typename EntryT>
  inline void painter_draw_entry_dispatch(neui_render_backend_t* backend,
                                          neui_render_ctx_t ctx,
                                          EntryT* entry, uint32_t frame,
                                          float x, float y, float w, float h,
                                          uint32_t tint)
  {
    if (frame == k_draw_asset_whole)
      painter_draw_entry_cached(backend, ctx, entry, x, y, w, h, tint);
    else
      painter_draw_entry_frame_cached(backend, ctx, entry, frame, x, y, w, h, tint);
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
                          x, y, w, h, k_draw_asset_whole, 0xFFFFFFFFu);
  }

  // Frame-aware draw: renders cell `frame` of a filmstrip asset. On an asset
  // with no frame layout this draws the whole bitmap (== draw_asset).
  inline void painter_draw_asset_frame(neui_painter_t* p, neui_asset_t asset,
                                        uint32_t frame,
                                        float x, float y, float w, float h)
  {
    if (!p || !p->draw_asset_thunk) return;
    // k_draw_asset_whole (UINT32_MAX) is the "draw the whole bitmap" sentinel.
    // A caller asking to draw that exact frame index means the LAST frame, so
    // nudge it onto the cell path (filmstrip_src_rect clamps any out-of-range
    // index to the last cell) instead of silently drawing the whole strip.
    if (frame == k_draw_asset_whole) frame = k_draw_asset_whole - 1u;
    p->draw_asset_thunk(p->host_token, p->backend, p->ctx, asset,
                          x, y, w, h, frame, 0xFFFFFFFFu);
  }

  // Tinted variants - bypass the public painter API (which does not expose
  // `tint`) and reach the thunk directly. Used by the compound asset-layer
  // paint path; clients of NEUI_W_CUSTOMDRAW use k_painter_api.draw_asset /
  // draw_asset_frame.
  inline void painter_draw_asset_tinted(neui_painter_t* p, neui_asset_t asset,
                                          float x, float y, float w, float h,
                                          uint32_t tint)
  {
    if (!p || !p->draw_asset_thunk) return;
    p->draw_asset_thunk(p->host_token, p->backend, p->ctx, asset,
                          x, y, w, h, k_draw_asset_whole, tint);
  }

  inline void painter_draw_asset_frame_tinted(neui_painter_t* p, neui_asset_t asset,
                                               uint32_t frame,
                                               float x, float y, float w, float h,
                                               uint32_t tint)
  {
    if (!p || !p->draw_asset_thunk) return;
    // See painter_draw_asset_frame: keep the whole-bitmap sentinel out of the
    // frame value domain so frame == UINT32_MAX clamps to the last cell.
    if (frame == k_draw_asset_whole) frame = k_draw_asset_whole - 1u;
    p->draw_asset_thunk(p->host_token, p->backend, p->ctx, asset,
                          x, y, w, h, frame, tint);
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
    painter_draw_asset_frame,
  };

} // namespace neui_detail
