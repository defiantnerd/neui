#pragma once

#include <cstring>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <neui/d/renderer.h>
#include <neui/d/assets.h>
#include <neui/d/painter.h>

#include "compound.h"
#include "behavior.h"
#include "painter.h"  // draw_asset_thunk_t + neui_painter + k_painter_api
#include "component_loader.h"  // BuiltComponent / ComponentDefaultAttr / ComponentParam

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

    // Populated for NEUI_ASSET_KIND_COMPOUND entries; null otherwise.
    std::unique_ptr<CompoundAsset> compound;

    // Populated for NEUI_ASSET_KIND_BEHAVIOR entries; null otherwise.
    std::unique_ptr<BehaviorAsset> behavior;

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

    // Allocate a slot from a file path (resolves @2x / @3x variants when
    // the requested scale > 1.0). Returns 0 on failure.
    uint32_t allocate_from_file(const std::string& name, float scale)
    {
      if (name.empty()) return 0;
      if (scale <= 0.0f) scale = 1.0f;

      std::string resolved = resolve_path(name, scale);
      if (resolved.empty()) return 0;

      uint32_t w_px = 0, h_px = 0;
      uint8_t* raw = Loader::load(resolved.c_str(), &w_px, &h_px);
      if (!raw || w_px == 0 || h_px == 0) {
        Loader::free_pixels(raw);
        return 0;
      }

      auto entry = std::make_unique<AssetEntry>();
      entry->kind      = NEUI_ASSET_KIND_BITMAP;
      entry->width_px  = w_px;
      entry->height_px = h_px;
      entry->scale     = scale_of_resolved(name, resolved);
      entry->pixels.assign(raw, raw + static_cast<size_t>(w_px) * h_px * 4);
      Loader::free_pixels(raw);

      return alloc_slot(std::move(entry));
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
      if (path.empty() || !backend || !backend->register_font_file) return 0;

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

      // Drop every cached per-window GPU upload of this surface so the
      // next draw_asset re-uploads from the new pixel buffer. The cache
      // key is (window-ctx), not surface_ctx - destroy_bitmap runs
      // against the window ctx that owns each cached bitmap.
      if (backend->destroy_bitmap) {
        for (auto& [other_ctx, cached] : entry->bitmaps)
          if (cached.bmp) backend->destroy_bitmap(other_ctx, cached.bmp);
      }
      entry->bitmaps.clear();
    }

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
      }
    }

    void clear(neui_render_backend_t* backend)
    {
      if (backend && backend->destroy_bitmap) {
        for (auto& entry : _handles) {
          if (!entry) continue;
          for (auto& [ctx, cached] : entry->bitmaps)
            if (cached.bmp) backend->destroy_bitmap(ctx, cached.bmp);
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

    // Resolves the best available path for the requested scale, with a
    // higher-res fallback so a deployment that ships only @2x (or @3x)
    // still loads on a 96-DPI display - the bitmap is downscaled at draw
    // time. Returns empty string if no suitable file is found.
    //   scale > 2.0 -> @3x -> @2x -> base
    //   scale > 1.0 -> @2x -> base -> @3x
    //   else        -> base -> @2x -> @3x
    static std::string resolve_path(const std::string& name, float scale)
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
          Loader::free_pixels(raw);
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
