#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <neui/d/renderer.h>
#include <neui/d/assets.h>
#include <neui/d/painter.h>
#include "../shared/win32/image_loader_win32.h"
#include "../shared/compound.h"
#include "../shared/behavior.h"
#include "../shared/painter.h"  // draw_asset_thunk_t + neui_painter

namespace win32_host
{
  // Slot-table asset entry. CPU-side pixels (BGRA8 premultiplied) + a
  // per-D2D-context GPU bitmap cache. Mirrors the xpl host's AssetEntry
  // shape (deliberately - if we later unify, the storage tier is already
  // identical). Lives in win32_host because the file-loading path uses
  // the win32-shared WIC loader directly rather than the xpl
  // platform-abstraction layer.
  //
  // W32CtxBitmap pairs the cached GPU handle with the backend's per-ctx
  // generation at the moment of upload. On lookup the draw thunk
  // compares against the backend's current generation; a mismatch means
  // the D2D target was recreated (D2DERR_RECREATE_TARGET) and the cached
  // handle is dangling - drop it and re-upload.
  struct W32CtxBitmap
  {
    void*    bmp        = nullptr;
    uint32_t generation = 0;
  };

  struct W32AssetEntry
  {
    neui_asset_kind_t    kind        = NEUI_ASSET_KIND_BITMAP;
    uint32_t             width_px    = 0;
    uint32_t             height_px   = 0;
    float                scale       = 1.0f;
    std::vector<uint8_t> pixels;
    std::unordered_map<neui_render_ctx_t, W32CtxBitmap> bitmaps;

    // Tinted-variant cache for the asset-layer "tint" prop. Slot count
    // is typically 1-3 (per distinct tint at draw time); linear scan.
    // Parallel to `bitmaps` - the untinted draw path is unchanged.
    std::vector<neui_detail::TintedCtxBitmap> tinted_bitmaps;

    // Populated for NEUI_ASSET_KIND_COMPOUND entries; null otherwise.
    std::unique_ptr<neui_detail::CompoundAsset> compound;

    // Populated for NEUI_ASSET_KIND_BEHAVIOR entries; null otherwise.
    std::unique_ptr<neui_detail::BehaviorAsset> behavior;

    // Populated for NEUI_ASSET_KIND_SURFACE entries; null otherwise.
    // Owns the off-screen D2D render context for the surface's lifetime;
    // `pixels` (above) carries the most recently rendered frame and
    // feeds back into the standard BITMAP upload path on draw.
    neui_render_ctx_t surface_ctx = nullptr;
  };

  // Session-scoped asset table backing the public neui_asset_api_t.
  // Header-only so the existing CMakeLists doesn't need to grow a new TU.
  class W32AssetManager
  {
  public:
    // Allocate a new slot from raw BGRA8 (premultiplied) pixels.
    // Returns 0 on failure. Slot 0 is intentionally unused so callers
    // can interpret 0 as "invalid".
    uint32_t allocate_bitmap(uint32_t width_px, uint32_t height_px,
                              const uint8_t* bgra_premul, float scale)
    {
      if (width_px == 0 || height_px == 0 || !bgra_premul) return 0;
      if (scale <= 0.0f) scale = 1.0f;

      auto entry = std::make_unique<W32AssetEntry>();
      entry->kind      = NEUI_ASSET_KIND_BITMAP;
      entry->width_px  = width_px;
      entry->height_px = height_px;
      entry->scale     = scale;
      entry->pixels.assign(bgra_premul,
                            bgra_premul + static_cast<size_t>(width_px) * height_px * 4);

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

    // Allocate a slot from a file path (resolves @2x / @3x variants
    // when the requested scale > 1.0). Returns 0 on failure.
    uint32_t allocate_from_file(const std::string& name, float scale)
    {
      if (name.empty()) return 0;
      if (scale <= 0.0f) scale = 1.0f;

      std::string resolved = resolve_path(name, scale);
      if (resolved.empty()) return 0;

      uint32_t w_px = 0, h_px = 0;
      uint8_t* raw = neui_detail::load_image_bgra8_w32(resolved.c_str(),
                                                         &w_px, &h_px);
      if (!raw || w_px == 0 || h_px == 0) {
        delete[] raw;
        return 0;
      }

      auto entry = std::make_unique<W32AssetEntry>();
      entry->kind      = NEUI_ASSET_KIND_BITMAP;
      entry->width_px  = w_px;
      entry->height_px = h_px;

      std::string base, ext;
      split_ext(name, base, ext);
      if      (resolved == base + "@3x" + ext) entry->scale = 3.0f;
      else if (resolved == base + "@2x" + ext) entry->scale = 2.0f;
      else                                      entry->scale = 1.0f;

      entry->pixels.assign(raw, raw + static_cast<size_t>(w_px) * h_px * 4);
      delete[] raw;

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

    // Allocate a slot holding an empty CompoundAsset. Mutated via
    // NEUI_API_COMPOUND. Returns 0 on failure.
    uint32_t allocate_compound()
    {
      auto entry = std::make_unique<W32AssetEntry>();
      entry->kind     = NEUI_ASSET_KIND_COMPOUND;
      entry->compound = std::make_unique<neui_detail::CompoundAsset>();

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

    // Allocate a slot holding an empty BehaviorAsset. Mutated via
    // NEUI_API_BEHAVIOR. Returns 0 on failure.
    uint32_t allocate_behavior()
    {
      auto entry = std::make_unique<W32AssetEntry>();
      entry->kind     = NEUI_ASSET_KIND_BEHAVIOR;
      entry->behavior = std::make_unique<neui_detail::BehaviorAsset>();

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

    // Allocate a SURFACE slot. Creates an off-screen render ctx via
    // backend->create_offscreen_context and reserves the BGRA8 pixel
    // buffer (zero-filled until the first paint). Returns 0 on
    // backends without off-screen support or on allocation failure.
    uint32_t allocate_surface(uint32_t width_px, uint32_t height_px,
                               float scale,
                               neui_render_backend_t* backend)
    {
      if (width_px == 0 || height_px == 0 || !backend
       || !backend->create_offscreen_context)
        return 0;
      if (scale <= 0.0f) scale = 1.0f;

      neui_render_ctx_t ctx = backend->create_offscreen_context(width_px, height_px, scale);
      if (!ctx) return 0;

      auto entry = std::make_unique<W32AssetEntry>();
      entry->kind        = NEUI_ASSET_KIND_SURFACE;
      entry->width_px    = width_px;
      entry->height_px   = height_px;
      entry->scale       = scale;
      entry->surface_ctx = ctx;
      entry->pixels.assign(static_cast<size_t>(width_px) * height_px * 4u, 0);

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

    // Drive a client paint callback against a SURFACE entry's off-screen
    // ctx, then read back pixels and drop cached per-window uploads so
    // the next draw_asset re-uploads. Mirrors the xpl asset manager's
    // shape; the painter handed to the callback uses the host's existing
    // draw_asset_thunk so nested draw_asset works.
    void paint_surface(uint32_t slot,
                       uint32_t clear_argb,
                       neui_surface_paint_fn fn,
                       void* user,
                       neui_render_backend_t* backend,
                       void* host_token,
                       neui_detail::draw_asset_thunk_t draw_asset_thunk)
    {
      if (slot == 0 || slot >= _handles.size() || !backend) return;
      auto& entry = _handles[slot];
      if (!entry || entry->kind != NEUI_ASSET_KIND_SURFACE || !entry->surface_ctx)
        return;
      if (!fn) return;

      neui_render_ctx_t ctx = entry->surface_ctx;
      if (backend->begin_frame) backend->begin_frame(ctx, clear_argb);
      if (backend->push_clip)
        backend->push_clip(ctx, 0.0f, 0.0f,
                            static_cast<float>(entry->width_px)  / entry->scale,
                            static_cast<float>(entry->height_px) / entry->scale);

      neui_painter painter{};
      painter.backend          = backend;
      painter.ctx              = ctx;
      painter.host_token       = host_token;
      painter.draw_asset_thunk = draw_asset_thunk;
      fn(&painter, &neui_detail::k_painter_api,
         static_cast<float>(entry->width_px)  / entry->scale,
         static_cast<float>(entry->height_px) / entry->scale,
         user);

      if (backend->pop_clip)  backend->pop_clip(ctx);
      if (backend->end_frame) backend->end_frame(ctx);

      if (backend->read_pixels_bgra)
        backend->read_pixels_bgra(ctx, entry->pixels.data());

      if (backend->destroy_bitmap) {
        for (auto& [other_ctx, cached] : entry->bitmaps)
          if (cached.bmp) backend->destroy_bitmap(other_ctx, cached.bmp);
      }
      entry->bitmaps.clear();
      // Tinted variants reference the pre-repaint pixels and are stale;
      // the next tinted draw uploads against the freshly read-back buffer.
      neui_detail::release_all_tinted_bitmaps(entry.get(), backend);
    }

    void release_slot(uint32_t slot, neui_render_backend_t* backend)
    {
      if (slot == 0 || slot >= _handles.size()) return;
      auto& entry = _handles[slot];
      if (!entry) return;
      if (backend && backend->destroy_bitmap) {
        for (auto& [ctx, cached] : entry->bitmaps)
          if (cached.bmp) backend->destroy_bitmap(ctx, cached.bmp);
      }
      neui_detail::release_all_tinted_bitmaps(entry.get(), backend);
      if (entry->surface_ctx && backend && backend->destroy_context) {
        backend->destroy_context(entry->surface_ctx);
        entry->surface_ctx = nullptr;
      }
      entry.reset();
      _free_slots.push_back(slot);
    }

    W32AssetEntry* get_slot(uint32_t slot)
    {
      if (slot == 0 || slot >= _handles.size()) return nullptr;
      return _handles[slot].get();
    }

    // Kind-polymorphic pixel readback for export paths (drag preview, future
    // file save, ...). Returns true and populates the out params for any
    // asset that resolves to displayable BGRA8 pixels - BITMAP today;
    // SURFACE will add one branch the day that kind lands (the field
    // layout is already identical). Returns false for COMPOUND, BEHAVIOR,
    // invalid slot, or any kind with no presentable pixels.
    //
    // The returned pointer is borrowed - valid only until the next mutating
    // call on this manager. Pixels are width_px * height_px * 4 bytes.
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
        // Both kinds back the pixel data the same way (BGRA8 premul).
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
        neui_detail::release_tinted_bitmaps_for_ctx(entry.get(), ctx, backend);
      }
    }

    void clear(neui_render_backend_t* backend)
    {
      if (backend && backend->destroy_bitmap) {
        for (auto& entry : _handles) {
          if (!entry) continue;
          for (auto& [ctx, cached] : entry->bitmaps)
            if (cached.bmp) backend->destroy_bitmap(ctx, cached.bmp);
          neui_detail::release_all_tinted_bitmaps(entry.get(), backend);
        }
      }
      // Release any SURFACE entries' off-screen ctxs before dropping
      // the table; the bitmap-cache loop above only walks window ctxs.
      if (backend && backend->destroy_context) {
        for (auto& entry : _handles) {
          if (entry && entry->surface_ctx) {
            backend->destroy_context(entry->surface_ctx);
            entry->surface_ctx = nullptr;
          }
        }
      }
      _handles.clear();
      _free_slots.clear();
    }

  private:
    std::vector<std::unique_ptr<W32AssetEntry>> _handles;
    std::vector<uint32_t>                       _free_slots;

    static void split_ext(const std::string& name,
                           std::string& base_out, std::string& ext_out)
    {
      auto dot = name.rfind('.');
      if (dot == std::string::npos) { base_out = name; ext_out.clear(); }
      else { base_out = name.substr(0, dot); ext_out = name.substr(dot); }
    }

    static std::string resolve_path(const std::string& name, float scale)
    {
      std::string base, ext;
      split_ext(name, base, ext);

      auto try_load = [](const std::string& p) -> bool {
        uint32_t w = 0, h = 0;
        uint8_t* raw = neui_detail::load_image_bgra8_w32(p.c_str(), &w, &h);
        if (!raw) return false;
        delete[] raw;
        return true;
      };

      if (scale > 2.0f) {
        if (try_load(base + "@3x" + ext)) return base + "@3x" + ext;
        if (try_load(base + "@2x" + ext)) return base + "@2x" + ext;
      } else if (scale > 1.0f) {
        if (try_load(base + "@2x" + ext)) return base + "@2x" + ext;
      }
      if (try_load(name)) return name;
      // Fallback: try higher-res variants the scale branch above skipped.
      if (scale <= 1.0f && try_load(base + "@2x" + ext)) return base + "@2x" + ext;
      if (scale <= 2.0f && try_load(base + "@3x" + ext)) return base + "@3x" + ext;
      return {};
    }
  };

} // namespace win32_host
