#pragma once

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

#include <neui/d/renderer.h>
#include <neui/d/assets.h>
#include <neui/d/painter.h>

#include "compound.h"
#include "behavior.h"
#include "painter.h"  // draw_asset_thunk_t + neui_painter + k_painter_api
#include "component_loader.h"  // BuiltComponent / ComponentDefaultAttr / ComponentParam
#include "filmstrip_recognize.h"  // FilmstripLayout + sidecar/filename discovery
#include "image_filter.h"  // image_gaussian_blur_bgra
#include "filter_graph.h"  // FilterAsset + evaluate_filter (SVG fe* engine)
#include "resource_provider.h"  // ResourceProvider (NEUI_API_RESOURCE_CLIENT)

// Session-scoped asset slot table shared by all three hosts. Each host
// previously carried a near-identical manager (xpl AssetManager, win32
// W32AssetManager, macOS MacOSAssetManager); the storage tier, slot
// allocation, kind handling, surface paint coordination and context
// lifecycle now live here once. The only per-host seam is the image
// loader, injected as a policy type:
//
//   struct Loader {
//     // Decode `path` into a heap BGRA8-premultiplied buffer; null on
//     // failure. Ownership returns to free_pixels.
//     static uint8_t* load(const char* path, uint32_t* w_px, uint32_t* h_px);
//     // Same, from encoded bytes already in memory - the client resource
//     // provider path (NEUI_API_RESOURCE_CLIENT) has no path to hand over.
//     static uint8_t* load_memory(const uint8_t* data, size_t len,
//                                 uint32_t* w_px, uint32_t* h_px);
//     static void     free_pixels(uint8_t* p);
//   };
//
// ODR-safe: header-only, all methods implicitly inline via the template.
// Thread-safety: not required (single-threaded UI).

namespace neui_detail
{
  // CtxBitmap pairs the cached GPU handle with the backend's per-ctx
  // generation counter at the moment of upload. On lookup the draw path
  // compares it against the backend's current generation: if the backend
  // has had to recreate the underlying device (D2DERR_RECREATE_TARGET),
  // the cached handle is dangling and is re-uploaded against the new
  // target. Backends without device-loss (CG, null) return a constant so
  // the check is free.
  struct CtxBitmap
  {
    void*    bmp        = nullptr;
    uint32_t generation = 0;
  };

  // Frame-strip ("filmstrip" / "stitchmap" / "sprite strip") layout for a
  // BITMAP / SURFACE asset whose pixels pack N evenly-spaced frames in a
  // cols x rows row-major grid (the audio-plugin convention: a single column
  // of N stacked frames, value -> frame). Present (non-null on AssetEntry)
  // only when the asset has been tagged via set_frame_layout; the kind stays
  // BITMAP / SURFACE so every other consumer treats it as an ordinary
  // bitmap. The per-cell source rect is computed at draw time by
  // filmstrip_src_rect (which tiles the full bitmap dimension exactly);
  // frame_w_px / frame_h_px are the nominal (floored) cell size kept for the
  // fit check + frame_info queries, not the draw-time pitch.
  struct FilmstripInfo
  {
    uint32_t frame_count = 0;   // cols * rows
    uint32_t cols        = 1;   // grid columns (vertical strip = 1)
    uint32_t rows        = 0;   // grid rows
    uint32_t gutter_px   = 0;   // physical px between cells (0 = tight pack)
    uint32_t frame_w_px  = 0;   // nominal (floored) physical px per cell
    uint32_t frame_h_px  = 0;
  };

  // A single loaded asset: kind discriminator, CPU pixels (for bitmap
  // kinds) and per-render-context GPU bitmap cache. Future kinds (SVG /
  // vector) will reuse the slot table but populate a different storage
  // tier instead of `pixels`.
  struct AssetEntry
  {
    neui_asset_kind_t    kind        = NEUI_ASSET_KIND_BITMAP;
    uint32_t             width_px    = 0;   // physical pixel dimensions (bitmap only)
    uint32_t             height_px   = 0;
    float                scale       = 1.0f; // HiDPI factor loaded (1, 2, or 3)
    std::vector<uint8_t> pixels;             // BGRA8 premultiplied, width_px*height_px*4

    // Backend bitmap cache: one GPU resource per render context. Tinted
    // draws of the same asset reuse the same upload - the backend's
    // draw_bitmap tint param handles colourisation at draw time, so the
    // per-(asset, ctx) entry stays single-source-of-truth.
    std::unordered_map<neui_render_ctx_t, CtxBitmap> bitmaps;

    // Populated when a BITMAP / SURFACE asset has been tagged as a frame
    // strip via set_frame_layout; null = ordinary single-image bitmap.
    std::unique_ptr<FilmstripInfo> filmstrip;

    // Populated for NEUI_ASSET_KIND_COMPOUND entries; null otherwise.
    std::unique_ptr<CompoundAsset> compound;

    // Populated for NEUI_ASSET_KIND_BEHAVIOR entries; null otherwise.
    std::unique_ptr<BehaviorAsset> behavior;

    // Populated for NEUI_ASSET_KIND_FILTER entries; null otherwise.
    std::unique_ptr<FilterAsset> filter;

    // Populated for NEUI_ASSET_KIND_SURFACE entries; null otherwise.
    // Owns the off-screen render context for the surface's lifetime.
    // `pixels` (above) holds the most recently rendered frame, fed back
    // into the standard BITMAP draw path via the per-ctx bitmap cache.
    neui_render_ctx_t surface_ctx = nullptr;

    // Populated for NEUI_ASSET_KIND_FONT entries. The backend register token
    // (0 = none) drives unregister_font on release; font_family is the
    // family name read back from the font data (the name the client passes
    // to NEUI_ATTR_FONT_FAMILY / push_font). For the in-memory form the
    // owned byte copy lives in `pixels` above (kept alive so the backend's
    // FreeType / in-memory loader keeps a valid buffer for the token's
    // lifetime); the file form leaves `pixels` empty (the backend reads the
    // file itself). FONT entries never enter the bitmaps / surface_ctx
    // tiers and get_pixels_for_export rejects them.
    uint64_t          font_token  = 0;
    std::string       font_family;

    // Populated for NEUI_ASSET_KIND_COMPONENT entries. A component bundles a
    // COMPOUND (comp_compound) + a BEHAVIOR (comp_behavior) - handles into
    // this same store - plus a default-attr template stamped onto each
    // instance, a param manifest, and a default size. comp_owned_assets are
    // the path-loaded layer assets the component owns (assets handed in by a
    // client resolve_asset callback are borrowed and NOT listed here).
    // release_slot releases comp_compound / comp_behavior / comp_owned_assets.
    neui_asset_t                      comp_compound = asset_none;
    neui_asset_t                      comp_behavior = asset_none;
    std::vector<neui_asset_t>         comp_owned_assets;
    std::vector<ComponentDefaultAttr> comp_defaults;
    std::vector<ComponentParam>       comp_params;
    float                             comp_w = 0.0f;
    float                             comp_h = 0.0f;
    // Round-trip metadata for serialize_component (designer export).
    std::string                                      comp_name;
    std::vector<std::pair<std::string, std::string>> comp_asset_names;
    std::vector<std::pair<uint32_t, std::string>>    comp_asset_handle_names;
    std::vector<std::pair<uint32_t, FilmstripLayout>> comp_asset_frame_layouts;
  };

  template <typename Loader>
  class AssetStore
  {
  public:
    // Allocate a new slot from raw BGRA8 (premultiplied) pixels.
    // Returns 0 on failure. Slot 0 is intentionally unused so callers
    // can interpret 0 as "invalid"; callers pack the slot with the owning
    // session id to form neui_asset_t::id.
    uint32_t allocate_bitmap(uint32_t width_px, uint32_t height_px,
                              const uint8_t* bgra_premul, float scale)
    {
      if (width_px == 0 || height_px == 0 || !bgra_premul) return 0;
      if (scale <= 0.0f) scale = 1.0f;

      auto entry = std::make_unique<AssetEntry>();
      entry->kind      = NEUI_ASSET_KIND_BITMAP;
      entry->width_px  = width_px;
      entry->height_px = height_px;
      entry->scale     = scale;
      entry->pixels.assign(bgra_premul,
                            bgra_premul + static_cast<size_t>(width_px) * height_px * 4);
      return alloc_slot(std::move(entry));
    }

    // --- Image source resolution (cached) ----------------------------------
    //
    // Where a (name, scale) pair's bytes actually come from. CACHED, including
    // misses, because the callers sit on the paint path: the path-keyed tier
    // backing NEUI_W_IMAGE resolves once per IMAGE widget per frame, and
    // resolution probes the @Nx candidate ladder by *decoding* each candidate
    // (resolve_path below). Before this cache existed an IMAGE widget paid a
    // full image decode every single frame purely to answer "which variant?".
    //
    // Keyed on the scale BUCKET rather than the raw scale, because that is all
    // resolve_path's candidate order depends on - so 1.25 / 1.5 / 1.75 all share
    // one entry.
    //
    // Negative results are sticky (v0 decision), with three ways out:
    //   * a scale that lands in a DIFFERENT band resolves independently. Note
    //     this is genuinely narrower than "a DPI change re-resolves": 125% and
    //     200% are both band 1, so moving between those two displays reuses the
    //     cached outcome.
    //   * an explicit allocate_from_file (create_from_file and friends - a
    //     client-initiated load, not a per-frame resolve) RE-PROBES a cached
    //     miss, so a resource published late still appears. The per-frame
    //     path-keyed tier deliberately does not.
    //   * clear_image_routes() / clear() drop them outright.
    struct ImageRoute {
      bool        found       = false;
      // Bytes come from the client resource provider rather than the
      // filesystem (see probe_image_route).
      bool        from_client = false;
      // The resource name exactly as it was asked for: the string to re-ask the
      // provider with, and the base name the @Nx ladder resolves from.
      std::string name;
      // Resolved @Nx filesystem variant. Empty on a client route - those bytes
      // have no path, which is why `name` is kept separately.
      std::string file_path;
      // Stable key for derived path-keyed caches: the resolved path for a file
      // route, a synthetic per-band key for a client route (see
      // client_cache_key - the band has to be in it).
      std::string cache_key;
      // HiDPI factor of what decode_route yields.
      float       scale       = 1.0f;
      // The display scale this route was resolved FOR - the scale_hint to re-ask
      // the provider with, so the fetch sees the same request the probe did.
      float       req_scale   = 1.0f;
    };

    // resolve_path's candidate order depends only on which band `scale` is in.
    static int scale_bucket(float scale)
    {
      return scale > 2.0f ? 2 : (scale > 1.0f ? 1 : 0);
    }

    // `refresh_miss` re-probes an entry cached as a MISS (see the sticky-negative
    // note above); a cached HIT is always reused.
    const ImageRoute& image_route(const std::string& name, float scale,
                                  bool refresh_miss = false)
    {
      static const ImageRoute k_no_route;
      if (name.empty()) return k_no_route;
      if (scale <= 0.0f) scale = 1.0f;

      std::string key = route_key(name, scale_bucket(scale));
      auto it = _routes.find(key);
      if (it != _routes.end()) {
        if (it->second.found || !refresh_miss) return it->second;
        it->second = probe_image_route(name, scale);
        return it->second;
      }
      return _routes.emplace(std::move(key), probe_image_route(name, scale))
                    .first->second;
    }

    void clear_image_routes()
    {
      _routes.clear();
      _probe = ProbePixels{};
    }

    // Installed by the host at session-create time from
    // client->get_interface(NEUI_API_RESOURCE_CLIENT). Absent (the default),
    // every resolution below behaves exactly as it did before the interface
    // existed.
    void set_resource_provider(const ResourceProvider& p)
    {
      _provider = p;
      clear_image_routes();   // cached misses predate the provider
    }
    const ResourceProvider& resource_provider() const { return _provider; }

    // Decode whatever `route` designates into `out_px` (BGRA8 premultiplied),
    // reporting the pixel size and - in *scale_out - the HiDPI factor of what was
    // ACTUALLY decoded. On a client route that is the scale the provider declares
    // on THIS call, which need not be the one the probe saw: a provider is free
    // to answer the same request with a different variant, and the entry must
    // record the scale of the pixels it is holding or the bitmap draws at the
    // wrong logical size. Returns false on failure (out_px left empty).
    bool decode_route(const ImageRoute& route, std::vector<uint8_t>& out_px,
                      uint32_t* w_px, uint32_t* h_px, float* scale_out)
    {
      out_px.clear();
      if (!route.found) return false;

      uint32_t w = 0, h = 0;
      float    sc = route.scale;

      if (_probe.valid && _probe.key == route.cache_key) {
        // The probe already decoded exactly these pixels - take them rather
        // than pay a second decode (and, on a client route, a second provide).
        out_px = std::move(_probe.pixels);
        w  = _probe.w_px;
        h  = _probe.h_px;
        sc = _probe.scale;
        _probe = ProbePixels{};
      } else if (route.from_client) {
        const bool ok = _provider.with_bytes(
            NEUI_RESOURCE_KIND_IMAGE, route.name.c_str(), route.req_scale, nullptr,
            [&](const uint8_t* data, uint32_t len, float s) {
              uint32_t dw = 0, dh = 0;
              uint8_t* raw = Loader::load_memory(data, len, &dw, &dh);
              if (!adopt_pixels(raw, dw, dh, out_px)) return false;
              w = dw; h = dh; sc = s;
              return true;
            });
        if (!ok) {
          // The provider validated once at probe time but has now declined, or
          // handed over bytes that no longer decode. Fall back to the filesystem
          // instead of failing the load for the rest of the session: "a buggy
          // provider cannot shadow a good file" has to hold for every load, not
          // just for the one probe that decided the route.
          out_px.clear();
          DecodedImage      file_px;
          const std::string fallback = resolve_path(route.name, route.req_scale,
                                                    &file_px);
          if (fallback.empty() || file_px.pixels.empty()) return false;
          out_px = std::move(file_px.pixels);
          w  = file_px.w_px;
          h  = file_px.h_px;
          sc = scale_of_resolved(route.name, fallback);
        }
      } else {
        uint32_t dw = 0, dh = 0;
        uint8_t* raw = Loader::load(route.file_path.c_str(), &dw, &dh);
        if (!adopt_pixels(raw, dw, dh, out_px)) return false;
        w = dw; h = dh;
      }

      if (w_px)      *w_px      = w;
      if (h_px)      *h_px      = h;
      if (scale_out) *scale_out = sc;
      return true;
    }

    // Allocate a slot from a file path (resolves @2x / @3x variants when
    // the requested scale > 1.0). Returns 0 on failure.
    uint32_t allocate_from_file(const std::string& name, float scale)
    {
      if (name.empty()) return 0;
      if (scale <= 0.0f) scale = 1.0f;

      // refresh_miss: this is an explicit, client-initiated load, so a resource
      // that was missing earlier gets another chance (see the sticky-negative
      // note on ImageRoute). The per-frame tier in the xpl AssetManager does not
      // pass it.
      const ImageRoute& route = image_route(name, scale, /*refresh_miss=*/true);
      if (!route.found) return 0;

      auto     entry = std::make_unique<AssetEntry>();
      uint32_t w_px = 0, h_px = 0;
      float    got_scale = route.scale;
      if (!decode_route(route, entry->pixels, &w_px, &h_px, &got_scale)) return 0;

      entry->kind      = NEUI_ASSET_KIND_BITMAP;
      entry->width_px  = w_px;
      entry->height_px = h_px;
      entry->scale     = got_scale;
      return alloc_slot(std::move(entry));
    }

    // Tag a BITMAP / SURFACE slot with a frame-strip layout: a cols x rows
    // row-major grid of equal cells separated by gutter_px (vertical strip =
    // cols 1). Cell size is floor((dim - (n-1)*gutter) / n) so the grid is
    // guaranteed to stay within the bitmap bounds. Returns false (leaving the
    // asset an untagged plain bitmap) for a non-bitmap kind, cols/rows < 1, a
    // zero-size bitmap, or a grid that can't fit at least 1 px per cell - so a
    // mis-tag can never produce an out-of-bounds source rect. Re-tagging
    // overwrites a prior layout; cols == rows == 1 leaves a single-frame
    // entry (frame_count 1), still drawable via the frame path.
    bool set_frame_layout(uint32_t slot, uint32_t cols, uint32_t rows,
                          uint32_t gutter_px)
    {
      AssetEntry* e = get_slot(slot);
      if (!e) return false;
      if (e->kind != NEUI_ASSET_KIND_BITMAP && e->kind != NEUI_ASSET_KIND_SURFACE)
        return false;
      if (cols < 1 || rows < 1) return false;
      if (e->width_px == 0 || e->height_px == 0) return false;

      const uint64_t gutters_x = static_cast<uint64_t>(cols - 1) * gutter_px;
      const uint64_t gutters_y = static_cast<uint64_t>(rows - 1) * gutter_px;
      if (gutters_x >= e->width_px || gutters_y >= e->height_px) return false;
      const uint32_t fw = static_cast<uint32_t>((e->width_px  - gutters_x) / cols);
      const uint32_t fh = static_cast<uint32_t>((e->height_px - gutters_y) / rows);
      if (fw == 0 || fh == 0) return false;
      // frame_count must stay addressable as a uint32 - guard the product so a
      // pathological grid can't wrap to a small bogus count (the comment above
      // promises a mis-tag never yields an out-of-bounds source rect).
      const uint64_t fc = static_cast<uint64_t>(cols) * rows;
      if (fc > 0xFFFFFFFFull) return false;

      auto fs = std::make_unique<FilmstripInfo>();
      fs->cols        = cols;
      fs->rows        = rows;
      fs->gutter_px   = gutter_px;
      fs->frame_w_px  = fw;
      fs->frame_h_px  = fh;
      fs->frame_count = static_cast<uint32_t>(fc);
      e->filmstrip = std::move(fs);
      return true;
    }

    // Frame layout for a slot, or nullptr if the slot is invalid / untagged.
    // Borrowed; valid until the next mutating call.
    const FilmstripInfo* frame_info(uint32_t slot)
    {
      AssetEntry* e = get_slot(slot);
      return (e && e->filmstrip) ? e->filmstrip.get() : nullptr;
    }

    // Frame count for a slot, or 0 if it isn't a frame strip.
    uint32_t frame_count(uint32_t slot)
    {
      const FilmstripInfo* fs = frame_info(slot);
      return fs ? fs->frame_count : 0u;
    }

    // Physical-px source rect of frame `frame` within a tagged slot (row
    // major; frame clamps into [0, frame_count)). Returns false for an
    // untagged / invalid slot, leaving the out params untouched.
    bool frame_src_rect(uint32_t slot, uint32_t frame,
                        float* sx, float* sy, float* sw, float* sh)
    {
      AssetEntry* e = get_slot(slot);
      if (!e || !e->filmstrip) return false;
      const FilmstripInfo* fs = e->filmstrip.get();
      float x = 0, y = 0, w = 0, h = 0;
      filmstrip_src_rect(fs->frame_count, fs->cols, fs->rows,
                         e->width_px, e->height_px, fs->gutter_px,
                         frame, x, y, w, h);
      if (sx) *sx = x;
      if (sy) *sy = y;
      if (sw) *sw = w;
      if (sh) *sh = h;
      return true;
    }

    // Load a bitmap from a file (allocate_from_file, incl. @2x/@3x resolution)
    // and tag it with an explicit cols x rows (+ gutter) frame grid. On tag
    // failure the freshly-loaded slot is released so a partial untagged asset
    // never leaks. Returns 0 on load failure / unfittable grid.
    uint32_t allocate_filmstrip_grid_from_file(const std::string& name, float scale,
                                               uint32_t cols, uint32_t rows,
                                               uint32_t gutter_px,
                                               neui_render_backend_t* backend)
    {
      uint32_t slot = allocate_from_file(name, scale);
      if (slot == 0) return 0;
      if (!set_frame_layout(slot, cols, rows, gutter_px)) {
        release_slot(slot, backend);
        return 0;
      }
      return slot;
    }

    // Convenience: load + tag a frame_count-frame strip - a single column
    // (horizontal == false) or single row (horizontal == true). When
    // frame_count == 0, DISCOVER the layout from a "<path>.json" / "<base>.json"
    // sidecar or a "<N>frames" / "-<N>" filename token (filmstrip_recognize.h);
    // `horizontal` then picks the axis for the filename branch. Returns 0 on
    // bad/undiscoverable layout, load failure, or an unfittable strip.
    uint32_t allocate_filmstrip_from_file(const std::string& name, float scale,
                                           uint32_t frame_count, bool horizontal,
                                           neui_render_backend_t* backend)
    {
      if (frame_count == 0) {
        FilmstripLayout lay;
        if (!filmstrip_discover_from_path(name, horizontal, lay, &_provider))
          return 0;
        return allocate_filmstrip_grid_from_file(name, scale, lay.cols, lay.rows,
                                                 lay.gutter, backend);
      }
      const uint32_t cols = horizontal ? frame_count : 1u;
      const uint32_t rows = horizontal ? 1u : frame_count;
      return allocate_filmstrip_grid_from_file(name, scale, cols, rows, 0, backend);
    }

    // Allocate a slot holding an empty CompoundAsset. Mutated via
    // NEUI_API_COMPOUND. Returns 0 on failure.
    uint32_t allocate_compound()
    {
      auto entry = std::make_unique<AssetEntry>();
      entry->kind     = NEUI_ASSET_KIND_COMPOUND;
      entry->compound = std::make_unique<CompoundAsset>();
      return alloc_slot(std::move(entry));
    }

    // Allocate a slot holding an empty BehaviorAsset. Mutated via
    // NEUI_API_BEHAVIOR. Returns 0 on failure.
    uint32_t allocate_behavior()
    {
      auto entry = std::make_unique<AssetEntry>();
      entry->kind     = NEUI_ASSET_KIND_BEHAVIOR;
      entry->behavior = std::make_unique<BehaviorAsset>();
      return alloc_slot(std::move(entry));
    }

    // Allocate a slot holding an empty FilterAsset (SVG fe* graph). Mutated
    // via NEUI_API_FILTER, applied to a SURFACE via apply_filter. Returns 0
    // on failure.
    uint32_t allocate_filter()
    {
      auto entry = std::make_unique<AssetEntry>();
      entry->kind   = NEUI_ASSET_KIND_FILTER;
      entry->filter = std::make_unique<FilterAsset>();
      return alloc_slot(std::move(entry));
    }

    // Allocate a NEUI_ASSET_KIND_COMPONENT slot wrapping a built component
    // (the compound + behavior + defaults + params + default size produced by
    // build_component). The component takes ownership of the sub-assets and
    // any path-loaded layer assets; release_slot releases them. Returns 0 if
    // the build failed or produced no compound.
    uint32_t allocate_component(const BuiltComponent& built)
    {
      if (!built.ok || built.compound.id == asset_none.id) return 0;
      auto entry = std::make_unique<AssetEntry>();
      entry->kind              = NEUI_ASSET_KIND_COMPONENT;
      entry->comp_compound     = built.compound;
      entry->comp_behavior     = built.behavior;
      entry->comp_owned_assets = built.owned_assets;
      entry->comp_defaults     = built.defaults;
      entry->comp_params       = built.params;
      entry->comp_w            = built.width;
      entry->comp_h            = built.height;
      entry->comp_name              = built.name;
      entry->comp_asset_names       = built.asset_names;
      entry->comp_asset_handle_names = built.asset_handle_names;
      entry->comp_asset_frame_layouts = built.asset_frame_layouts;
      return alloc_slot(std::move(entry));
    }

    // Register an in-memory font (NEUI_ASSET_KIND_FONT). Copies the bytes
    // into the entry (keeping them alive for the backend's loader), calls
    // backend->register_font, and on success stashes the token + resolved
    // family name. Returns 0 on bad args, on backends without font support,
    // or when the data is not a usable font.
    uint32_t allocate_font(const uint8_t* data, uint32_t len,
                            neui_render_backend_t* backend)
    {
      if (!data || len == 0 || !backend || !backend->register_font) return 0;

      auto entry = std::make_unique<AssetEntry>();
      entry->kind = NEUI_ASSET_KIND_FONT;
      entry->pixels.assign(data, data + len);  // own the bytes for the loader

      char     family[256] = { 0 };
      uint64_t token       = 0;
      if (!backend->register_font(entry->pixels.data(), len,
                                   family, sizeof(family), &token))
        return 0;
      entry->font_token  = token;
      entry->font_family = family;
      return alloc_slot(std::move(entry));
    }

    // Register a font from a file path (NEUI_ASSET_KIND_FONT) via
    // backend->register_font_file. The backend reads the file itself, so no
    // byte copy is held. Returns 0 on failure / no font support.
    uint32_t allocate_font_from_file(const std::string& path,
                                      neui_render_backend_t* backend)
    {
      if (path.empty() || !backend) return 0;

      // Client first: a client keeping its fonts in a container hands over
      // bytes, which is the in-memory registration path rather than the
      // backend's read-the-file-yourself one. A blob the backend rejects falls
      // through to the file below (decision 9).
      if (_provider.serves(NEUI_RESOURCE_KIND_FONT)) {
        uint32_t slot = 0;
        _provider.with_bytes(
            NEUI_RESOURCE_KIND_FONT, path.c_str(), 0.0f, nullptr,
            [&](const uint8_t* data, uint32_t len, float) {
              slot = allocate_font(data, len, backend);
              return slot != 0;
            });
        if (slot != 0) return slot;
      }

      if (!backend->register_font_file) return 0;

      auto entry = std::make_unique<AssetEntry>();
      entry->kind = NEUI_ASSET_KIND_FONT;

      char     family[256] = { 0 };
      uint64_t token       = 0;
      if (!backend->register_font_file(path.c_str(),
                                        family, sizeof(family), &token))
        return 0;
      entry->font_token  = token;
      entry->font_family = family;
      return alloc_slot(std::move(entry));
    }

    // Copy a FONT entry's resolved family name into out_buf (UTF-8,
    // NUL-terminated, truncated to cap). Returns the full length excluding
    // the NUL, or 0 for a non-FONT / invalid slot.
    uint32_t get_font_family(uint32_t slot, char* out_buf, uint32_t cap)
    {
      AssetEntry* e = get_slot(slot);
      if (!e || e->kind != NEUI_ASSET_KIND_FONT) return 0;
      const std::string& fam = e->font_family;
      if (out_buf && cap > 0) {
        uint32_t n = static_cast<uint32_t>(fam.size());
        if (n > cap - 1) n = cap - 1;
        if (n) std::memcpy(out_buf, fam.data(), n);
        out_buf[n] = '\0';
      }
      return static_cast<uint32_t>(fam.size());
    }

    // Allocate a SURFACE slot. Creates an off-screen render ctx via
    // backend->create_offscreen_context and reserves the BGRA8 pixel
    // buffer (zero-filled so the first draw_asset before any
    // paint_surface sees transparent black rather than uninitialised
    // memory). Returns 0 on backends without off-screen support (null)
    // or on allocation failure.
    uint32_t allocate_surface(uint32_t width_px, uint32_t height_px,
                               float scale,
                               neui_render_backend_t* backend)
    {
      if (width_px == 0 || height_px == 0 || !backend
       || !backend->create_offscreen_context)
        return 0;
      if (scale <= 0.0f) scale = 1.0f;

      neui_render_ctx_t ctx =
        backend->create_offscreen_context(width_px, height_px, scale);
      if (!ctx) return 0;

      auto entry = std::make_unique<AssetEntry>();
      entry->kind        = NEUI_ASSET_KIND_SURFACE;
      entry->width_px    = width_px;
      entry->height_px   = height_px;
      entry->scale       = scale;
      entry->surface_ctx = ctx;
      entry->pixels.assign(static_cast<size_t>(width_px) * height_px * 4u, 0);
      return alloc_slot(std::move(entry));
    }

    // Drive a client paint callback against a SURFACE entry's off-screen
    // ctx. The painter handed to the callback uses the same host_token +
    // draw_asset_thunk the host installs in WIDGET_PAINT dispatch, so
    // nested draw_asset calls work transparently. On return, the
    // surface's CPU pixel buffer holds the freshly rendered frame and
    // every cached per-window GPU upload of this asset has been dropped
    // so subsequent draw_asset calls re-upload. No-op on non-SURFACE
    // slots, on null fn, or on backends without off-screen support.
    void paint_surface(uint32_t slot,
                       uint32_t clear_argb,
                       neui_surface_paint_fn fn,
                       void* user,
                       neui_render_backend_t* backend,
                       void* host_token,
                       draw_asset_thunk_t draw_asset_thunk)
    {
      if (slot == 0 || slot >= _handles.size() || !backend) return;
      auto& entry = _handles[slot];
      if (!entry || entry->kind != NEUI_ASSET_KIND_SURFACE || !entry->surface_ctx)
        return;
      if (!fn) return;

      neui_render_ctx_t ctx = entry->surface_ctx;

      // Drive a complete frame on the off-screen ctx using the same calls
      // the windowed paint path uses - the backend's begin_frame/end_frame
      // already reset the path / transform / alpha / font stacks, so a
      // missing pop in one paint_surface can't leak into the next.
      if (backend->begin_frame) backend->begin_frame(ctx, clear_argb);
      if (backend->push_clip)
        backend->push_clip(ctx, 0.0f, 0.0f,
                            static_cast<float>(entry->width_px)  / entry->scale,
                            static_cast<float>(entry->height_px) / entry->scale);

      // Stack-allocate the painter shim - same layout the WIDGET_PAINT
      // dispatch site builds, so the client can call any painter_api
      // method (including nested draw_asset on other handles).
      neui_painter painter{};
      painter.backend          = backend;
      painter.ctx              = ctx;
      painter.host_token       = host_token;
      painter.draw_asset_thunk = draw_asset_thunk;
      fn(&painter, &k_painter_api,
         static_cast<float>(entry->width_px)  / entry->scale,
         static_cast<float>(entry->height_px) / entry->scale,
         user);

      if (backend->pop_clip)  backend->pop_clip(ctx);
      if (backend->end_frame) backend->end_frame(ctx);

      // Pull the freshly rendered pixels into the CPU buffer.
      if (backend->read_pixels_bgra)
        backend->read_pixels_bgra(ctx, entry->pixels.data());

      // Drop every cached per-window GPU upload of this surface so the next
      // draw_asset re-uploads from the new pixel buffer (the cache key is the
      // window ctx, not surface_ctx - destroy_bitmap runs against the window
      // ctx that owns each cached bitmap). Shared with the filter ops below.
      drop_surface_gpu_cache(*entry, backend);
    }

    // Drop every cached per-window GPU upload of a SURFACE slot so the next
    // draw_asset re-uploads from the (just-mutated) CPU pixel buffer. Shared
    // tail of paint_surface / the filter ops below.
    void drop_surface_gpu_cache(AssetEntry& entry, neui_render_backend_t* backend)
    {
      if (backend && backend->destroy_bitmap) {
        for (auto& [other_ctx, cached] : entry.bitmaps)
          if (cached.bmp) backend->destroy_bitmap(other_ctx, cached.bmp);
      }
      entry.bitmaps.clear();
    }

    // Evaluate a built FilterAsset in place over a SURFACE slot's pixels at the
    // surface's backing scale, then drop the GPU cache. The shared tail of
    // apply_filter + every surface_* convenience method. No-op unless `slot` is
    // a SURFACE with populated pixels.
    void apply_filter_asset(uint32_t slot, const FilterAsset& fa,
                            neui_render_backend_t* backend)
    {
      if (slot == 0 || slot >= _handles.size()) return;
      auto& entry = _handles[slot];
      if (!entry || entry->kind != NEUI_ASSET_KIND_SURFACE) return;
      if (entry->pixels.empty() || entry->width_px == 0 || entry->height_px == 0) return;
      const float s = entry->scale > 0.0f ? entry->scale : 1.0f;
      evaluate_filter(fa, entry->pixels.data(), entry->width_px, entry->height_px, s);
      drop_surface_gpu_cache(*entry, backend);
    }

    // Evaluate a stored FILTER graph over a SURFACE (the public apply_filter).
    // No-op unless both slots are valid (SURFACE with pixels + FILTER graph).
    void apply_filter(uint32_t surface_slot, uint32_t filter_slot,
                      neui_render_backend_t* backend)
    {
      if (filter_slot == 0 || filter_slot >= _handles.size()) return;
      auto& filt = _handles[filter_slot];
      if (!filt || filt->kind != NEUI_ASSET_KIND_FILTER || !filt->filter) return;
      apply_filter_asset(surface_slot, *filt->filter, backend);
    }

    // Convenience surface filters - each builds the matching recipe graph
    // (filter_graph.h filter_build_*) and runs it through apply_filter_asset,
    // so there is one code path. All distances are LOGICAL px. No-op on
    // non-SURFACE slots.
    void blur_surface(uint32_t slot, float sigma_x, float sigma_y, neui_render_backend_t* backend)
    { FilterAsset fa; filter_build_blur(fa, sigma_x, sigma_y); apply_filter_asset(slot, fa, backend); }

    void drop_shadow_surface(uint32_t slot, float dx, float dy, float sigma,
                             uint32_t shadow_argb, neui_render_backend_t* backend)
    { FilterAsset fa; filter_build_drop_shadow(fa, dx, dy, sigma, shadow_argb); apply_filter_asset(slot, fa, backend); }

    void inner_shadow_surface(uint32_t slot, float dx, float dy, float sigma,
                              uint32_t shadow_argb, neui_render_backend_t* backend)
    { FilterAsset fa; filter_build_inner_shadow(fa, dx, dy, sigma, shadow_argb); apply_filter_asset(slot, fa, backend); }

    void glow_surface(uint32_t slot, float sigma, uint32_t glow_argb, neui_render_backend_t* backend)
    { FilterAsset fa; filter_build_glow(fa, sigma, glow_argb); apply_filter_asset(slot, fa, backend); }

    void tint_surface(uint32_t slot, uint32_t argb, neui_render_backend_t* backend)
    { FilterAsset fa; filter_build_tint(fa, argb); apply_filter_asset(slot, fa, backend); }

    void desaturate_surface(uint32_t slot, float amount, neui_render_backend_t* backend)
    { FilterAsset fa; filter_build_desaturate(fa, amount); apply_filter_asset(slot, fa, backend); }

    void elevation_surface(uint32_t slot, float level, neui_render_backend_t* backend)
    { FilterAsset fa; filter_build_elevation(fa, level); apply_filter_asset(slot, fa, backend); }

    void bevel_surface(uint32_t slot, float dx, float dy, float sigma,
                       uint32_t light_argb, uint32_t dark_argb, neui_render_backend_t* backend)
    { FilterAsset fa; filter_build_bevel(fa, dx, dy, sigma, light_argb, dark_argb); apply_filter_asset(slot, fa, backend); }

    // Release the slot. CPU pixels freed immediately; GPU caches dropped.
    void release_slot(uint32_t slot, neui_render_backend_t* backend)
    {
      if (slot == 0 || slot >= _handles.size()) return;
      auto& entry = _handles[slot];
      if (!entry) return;
      if (backend && backend->destroy_bitmap) {
        for (auto& [ctx, cached] : entry->bitmaps)
          if (cached.bmp) backend->destroy_bitmap(ctx, cached.bmp);
      }
      if (entry->surface_ctx && backend && backend->destroy_context) {
        backend->destroy_context(entry->surface_ctx);
        entry->surface_ctx = nullptr;
      }
      // Unregister FONT entries BEFORE freeing the byte buffer below - the
      // backend's FreeType face / in-memory loader still references
      // entry->pixels until unregister_font runs.
      if (entry->font_token && backend && backend->unregister_font) {
        backend->unregister_font(entry->font_token);
        entry->font_token = 0;
      }
      // Release a COMPONENT's owned sub-assets (compound + behavior + path-
      // loaded layer assets). Slot-only: the store is session-agnostic, so the
      // low 16 bits of each handle are the slot (the host owns the session
      // bits). Borrowed assets (from a client resolve_asset callback) are not
      // listed in comp_owned_assets, so they're left alone. Snapshot the
      // handles before the recursive calls (which mutate _handles/_free_slots).
      if (entry->kind == NEUI_ASSET_KIND_COMPONENT) {
        neui_asset_t sub_c = entry->comp_compound;
        neui_asset_t sub_b = entry->comp_behavior;
        std::vector<neui_asset_t> owned = entry->comp_owned_assets;
        if (sub_c.id != asset_none.id) release_slot(sub_c.id & 0xffffu, backend);
        if (sub_b.id != asset_none.id) release_slot(sub_b.id & 0xffffu, backend);
        for (auto a : owned)
          if (a.id != asset_none.id) release_slot(a.id & 0xffffu, backend);
      }
      entry.reset();
      _free_slots.push_back(slot);
    }

    // Look up an asset by slot. Returns nullptr if the slot is unused or
    // out of range. Caller must not retain the pointer past the next
    // mutating call.
    AssetEntry* get_slot(uint32_t slot)
    {
      if (slot == 0 || slot >= _handles.size()) return nullptr;
      return _handles[slot].get();
    }

    // Kind-polymorphic pixel readback for export paths (drag preview,
    // future file save, ...). Returns true and populates the out params
    // for any asset that resolves to displayable BGRA8 pixels (BITMAP /
    // SURFACE - both back the pixel data the same way). Returns false for
    // COMPOUND, BEHAVIOR, invalid slot, or any kind with no presentable
    // pixels. Pixels are width_px * height_px * 4 bytes; pointer is
    // borrowed (valid until the next mutating call).
    bool get_pixels_for_export(uint32_t slot,
                                const uint8_t** out_bgra,
                                uint32_t* out_w_px,
                                uint32_t* out_h_px,
                                float*    out_scale) const
    {
      if (slot == 0 || slot >= _handles.size()) return false;
      const auto& entry = _handles[slot];
      if (!entry) return false;
      switch (entry->kind) {
      case NEUI_ASSET_KIND_BITMAP:
      case NEUI_ASSET_KIND_SURFACE:
        if (entry->pixels.empty()) return false;
        if (out_bgra)  *out_bgra  = entry->pixels.data();
        if (out_w_px)  *out_w_px  = entry->width_px;
        if (out_h_px)  *out_h_px  = entry->height_px;
        if (out_scale) *out_scale = entry->scale;
        return true;
      default:
        return false;
      }
    }

    // --- Context lifecycle --------------------------------------------------

    // A window render context is dying: drop its cached uploads from every
    // entry. SURFACE entries' own off-screen ctxs are untouched - only
    // their cached upload of the dying window ctx goes.
    void release_context(neui_render_ctx_t ctx, neui_render_backend_t* backend)
    {
      if (!ctx || !backend || !backend->destroy_bitmap) return;
      for (auto& entry : _handles) {
        if (!entry) continue;
        auto it = entry->bitmaps.find(ctx);
        if (it != entry->bitmaps.end()) {
          if (it->second.bmp) backend->destroy_bitmap(ctx, it->second.bmp);
          entry->bitmaps.erase(it);
        }
        // QR compound layers hold their own per-(ctx) uploads of the symbol
        // bitmap; free them for this ctx too.
        if (entry->compound)
          neui_detail::compound_release_ctx_bitmaps(
            *entry->compound, ctx,
            [&](void* bmp) { backend->destroy_bitmap(ctx, bmp); });
      }
    }

    void clear(neui_render_backend_t* backend)
    {
      if (backend && backend->destroy_bitmap) {
        for (auto& entry : _handles) {
          if (!entry) continue;
          for (auto& [ctx, cached] : entry->bitmaps)
            if (cached.bmp) backend->destroy_bitmap(ctx, cached.bmp);
          // Free QR compound layers' per-(ctx) symbol uploads across all ctxs.
          if (entry->compound) {
            for (auto& layer : entry->compound->layers) {
              if (!layer) continue;
              for (auto& sym : layer->qr_cache) {
                if (!sym) continue;
                for (auto& [ctx, cached] : sym->bitmaps)
                  if (cached.bmp) backend->destroy_bitmap(ctx, cached.bmp);
                sym->bitmaps.clear();
              }
            }
          }
        }
      }
      // Release any SURFACE entries' off-screen ctxs before dropping the
      // table; destroy_bitmap above only walks cached window-ctx uploads.
      if (backend && backend->destroy_context) {
        for (auto& entry : _handles) {
          if (entry && entry->surface_ctx) {
            backend->destroy_context(entry->surface_ctx);
            entry->surface_ctx = nullptr;
          }
        }
      }
      // Unregister any FONT entries (still referencing entry->pixels) before
      // the table - and the byte buffers - are dropped.
      if (backend && backend->unregister_font) {
        for (auto& entry : _handles) {
          if (entry && entry->font_token) {
            backend->unregister_font(entry->font_token);
            entry->font_token = 0;
          }
        }
      }
      _handles.clear();
      _free_slots.clear();
      // Resolution outcomes describe the assets just torn down, so they go with
      // them - otherwise a full asset reset could not recover a sticky MISS for
      // a resource that has since appeared.
      clear_image_routes();
    }

    // --- Path helpers (shared with derived path-keyed caches) --------------

    // Splits "dir/file.ext" into {"dir/file", ".ext"}.
    static void split_ext(const std::string& name,
                           std::string& base_out, std::string& ext_out)
    {
      auto dot = name.rfind('.');
      if (dot == std::string::npos) { base_out = name; ext_out.clear(); }
      else { base_out = name.substr(0, dot); ext_out = name.substr(dot); }
    }

    // A decoded BGRA8-premultiplied image, owned by a vector so no raw
    // Loader buffer is ever in flight across a return.
    struct DecodedImage {
      std::vector<uint8_t> pixels;
      uint32_t             w_px = 0, h_px = 0;
    };

    // Move a Loader buffer into `out` and release it. Returns false for a failed
    // or zero-size decode (still releasing whatever came back).
    static bool adopt_pixels(uint8_t* raw, uint32_t w, uint32_t h,
                             std::vector<uint8_t>& out)
    {
      if (!raw || w == 0 || h == 0) { Loader::free_pixels(raw); return false; }
      out.assign(raw, raw + static_cast<size_t>(w) * h * 4);
      Loader::free_pixels(raw);
      return true;
    }

    // Resolves the best available path for the requested scale, with a
    // higher-res fallback so a deployment that ships only @2x (or @3x)
    // still loads on a 96-DPI display - the bitmap is downscaled at draw
    // time. Returns empty string if no suitable file is found.
    //   scale > 2.0 -> @3x -> @2x -> base
    //   scale > 1.0 -> @2x -> base -> @3x
    //   else        -> base -> @2x -> @3x
    // Existence is tested by DECODING a candidate (there is no stat() in the
    // Loader policy), so pass `keep` to be handed the winning candidate's pixels
    // instead of throwing them away - that is what keeps a cold load down to one
    // decode rather than one to probe plus one for real.
    static std::string resolve_path(const std::string& name, float scale,
                                    DecodedImage* keep = nullptr)
    {
      std::string base, ext;
      split_ext(name, base, ext);

      auto try_path = [&](const std::string& suffix) -> std::string {
        return base + suffix + ext;
      };

      std::vector<std::string> candidates;
      if (scale > 2.0f) {
        candidates = { try_path("@3x"), try_path("@2x"), name };
      } else if (scale > 1.0f) {
        candidates = { try_path("@2x"), name, try_path("@3x") };
      } else {
        candidates = { name, try_path("@2x"), try_path("@3x") };
      }

      for (auto& p : candidates) {
        uint32_t w = 0, h = 0;
        uint8_t* raw = Loader::load(p.c_str(), &w, &h);
        if (raw) {
          if (keep) {
            if (adopt_pixels(raw, w, h, keep->pixels)) {
              keep->w_px = w;
              keep->h_px = h;
            }
          } else {
            Loader::free_pixels(raw);
          }
          return p;
        }
      }
      return {};
    }

    // The actual HiDPI factor of the variant resolve_path picked.
    static float scale_of_resolved(const std::string& name,
                                    const std::string& resolved)
    {
      std::string base, ext;
      split_ext(name, base, ext);
      if (resolved == base + "@3x" + ext) return 3.0f;
      if (resolved == base + "@2x" + ext) return 2.0f;
      return 1.0f;
    }

  protected:
    // One resolution attempt for a (name, scale bucket) pair. Client first
    // (decision 1 in plans/client-resource-provider.md): for a client whose
    // assets live in a container the filesystem ladder below is a guaranteed
    // miss - up to three failed decodes on paths that will never exist - and on
    // an embedded target there may be no filesystem at all.
    //
    // The client probe DECODES what it gets back, which is what makes decision 9
    // work: bytes that do not decode are treated as a miss and resolution
    // continues to the filesystem, so a broken provider cannot shadow a good
    // file. Those pixels are PARKED (park_probe) and handed to the first
    // decode_route for the same route, so a cold load costs one provide() and
    // one decode - and none per frame, which is the cost that actually mattered.
    ImageRoute probe_image_route(const std::string& name, float scale)
    {
      ImageRoute r;
      r.name      = name;
      r.req_scale = scale;
      _probe = ProbePixels{};   // anything a previous probe parked is stale now

      if (_provider.serves(NEUI_RESOURCE_KIND_IMAGE)) {
        DecodedImage got;
        float        got_scale = 1.0f;
        const bool usable = _provider.with_bytes(
            NEUI_RESOURCE_KIND_IMAGE, name.c_str(), scale, nullptr,
            [&](const uint8_t* data, uint32_t len, float s) {
              uint32_t w = 0, h = 0;
              uint8_t* raw = Loader::load_memory(data, len, &w, &h);
              if (!adopt_pixels(raw, w, h, got.pixels)) return false;
              got.w_px  = w;
              got.h_px  = h;
              got_scale = s;
              return true;
            });
        if (usable) {
          r.found       = true;
          r.from_client = true;
          r.cache_key   = client_cache_key(name, scale_bucket(scale));
          r.scale       = got_scale;
          park_probe(r.cache_key, std::move(got), got_scale);
          return r;
        }
      }

      DecodedImage      file_px;
      const std::string resolved = resolve_path(name, scale, &file_px);
      if (!resolved.empty()) {
        r.found     = true;
        r.file_path = resolved;
        r.cache_key = resolved;
        r.scale     = scale_of_resolved(name, resolved);
        park_probe(r.cache_key, std::move(file_px), r.scale);
      }
      return r;
    }

    // Cache key for a route whose bytes came from the client: those bytes have no
    // path, and the scale BAND has to be part of the key. A provider may
    // legitimately answer one name with different pixels per band (that is what
    // scale_hint is for), and the derived path-keyed caches store one AssetEntry
    // per key - drop the band and every band after the first would be served the
    // first one's bitmap at the wrong resolution.
    static std::string client_cache_key(const std::string& name, int bucket)
    {
      return std::string("\x01") + "client\x01" + static_cast<char>('0' + bucket)
           + '\x01' + name;   // '\x01' cannot occur in a path
    }

    // (name, scale bucket) -> resolution outcome key. Folded into one string so
    // the map can hash rather than compare strings down a tree - this sits on the
    // per-frame paint path.
    static std::string route_key(const std::string& name, int bucket)
    {
      return name + '\x01' + static_cast<char>('0' + bucket);
    }

    // Pixels a probe has already decoded, waiting for the first decode_route of
    // that same route. probe_image_route must decode to answer "are these bytes
    // usable?" and resolve_path must decode to answer "does this candidate
    // exist?"; parking the result is what stops the real load decoding a second
    // time. At most ONE image is held, and it is dropped by the next probe, by
    // the decode that consumes it, or by clear_image_routes().
    struct ProbePixels {
      bool                 valid = false;
      std::string          key;                 // ImageRoute::cache_key it is for
      std::vector<uint8_t> pixels;              // BGRA8 premultiplied
      uint32_t             w_px = 0, h_px = 0;
      float                scale = 1.0f;
    };
    ProbePixels _probe;

    void park_probe(const std::string& key, DecodedImage&& img, float scale)
    {
      if (img.pixels.empty() || img.w_px == 0 || img.h_px == 0) return;
      _probe.valid  = true;
      _probe.key    = key;
      _probe.pixels = std::move(img.pixels);
      _probe.w_px   = img.w_px;
      _probe.h_px   = img.h_px;
      _probe.scale  = scale;
    }

    // route_key(name, bucket) -> resolution outcome, misses included.
    std::unordered_map<std::string, ImageRoute> _routes;

    // Optional client byte provider; empty unless the host installed one.
    ResourceProvider _provider;

    // Slot table for handle-based assets (public neui_asset_api_t).
    // _handles[0] is intentionally unused so slot 0 maps to "invalid".
    std::vector<std::unique_ptr<AssetEntry>> _handles;
    std::vector<uint32_t>                    _free_slots;

    uint32_t alloc_slot(std::unique_ptr<AssetEntry> entry)
    {
      if (_handles.empty()) _handles.emplace_back(nullptr);
      uint32_t slot;
      if (!_free_slots.empty()) {
        slot = _free_slots.back();
        _free_slots.pop_back();
        _handles[slot] = std::move(entry);
      } else {
        slot = static_cast<uint32_t>(_handles.size());
        _handles.emplace_back(std::move(entry));
      }
      return slot;
    }
  };

} // namespace neui_detail
