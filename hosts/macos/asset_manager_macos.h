#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <neui/d/renderer.h>
#include <neui/d/assets.h>
#include "../shared/macos/image_loader_macos.h"
#include "../shared/compound.h"
#include "../shared/behavior.h"

namespace macos_host
{
  // Slot-table asset entry. CPU-side pixels (BGRA8 premultiplied) + a
  // per-CG-context GPU bitmap cache. Mirrors hosts/win32/asset_manager_w32.h's
  // W32AssetEntry shape - CG bitmaps are device-independent (CGImageRef
  // doesn't bind to a render target the way D2D's ID2D1Bitmap does), but
  // we keep the per-ctx map for symmetry with the win32 path so the
  // painter draw_asset thunk shape stays identical across hosts.
  //
  // cg_get_context_generation() is a constant on CG (no device-loss path),
  // so the generation field is here only to match W32CtxBitmap's layout.
  struct MacOSCtxBitmap
  {
    void*    bmp        = nullptr;
    uint32_t generation = 0;
  };

  struct MacOSAssetEntry
  {
    neui_asset_kind_t    kind        = NEUI_ASSET_KIND_BITMAP;
    uint32_t             width_px    = 0;
    uint32_t             height_px   = 0;
    float                scale       = 1.0f;
    std::vector<uint8_t> pixels;
    std::unordered_map<neui_render_ctx_t, MacOSCtxBitmap> bitmaps;

    // Populated for NEUI_ASSET_KIND_COMPOUND entries; null otherwise.
    std::unique_ptr<neui_detail::CompoundAsset> compound;

    // Populated for NEUI_ASSET_KIND_BEHAVIOR entries; null otherwise.
    std::unique_ptr<neui_detail::BehaviorAsset> behavior;
  };

  // Session-scoped asset table backing the public neui_asset_api_t.
  // Header-only so the existing CMakeLists doesn't need to grow a new TU.
  class MacOSAssetManager
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

      auto entry = std::make_unique<MacOSAssetEntry>();
      entry->kind      = NEUI_ASSET_KIND_BITMAP;
      entry->width_px  = width_px;
      entry->height_px = height_px;
      entry->scale     = scale;
      entry->pixels.assign(bgra_premul,
                            bgra_premul + static_cast<size_t>(width_px) * height_px * 4);

      return alloc_slot(std::move(entry));
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
      uint8_t* raw = neui_detail::load_image_bgra8_macos(resolved.c_str(),
                                                          &w_px, &h_px);
      if (!raw || w_px == 0 || h_px == 0) {
        neui_detail::free_image_bgra8(raw);
        return 0;
      }

      auto entry = std::make_unique<MacOSAssetEntry>();
      entry->kind      = NEUI_ASSET_KIND_BITMAP;
      entry->width_px  = w_px;
      entry->height_px = h_px;

      std::string base, ext;
      split_ext(name, base, ext);
      if      (resolved == base + "@3x" + ext) entry->scale = 3.0f;
      else if (resolved == base + "@2x" + ext) entry->scale = 2.0f;
      else                                      entry->scale = 1.0f;

      entry->pixels.assign(raw, raw + static_cast<size_t>(w_px) * h_px * 4);
      neui_detail::free_image_bgra8(raw);

      return alloc_slot(std::move(entry));
    }

    // Allocate a slot holding an empty CompoundAsset. Mutated via
    // NEUI_API_COMPOUND. Returns 0 on failure.
    uint32_t allocate_compound()
    {
      auto entry = std::make_unique<MacOSAssetEntry>();
      entry->kind     = NEUI_ASSET_KIND_COMPOUND;
      entry->compound = std::make_unique<neui_detail::CompoundAsset>();
      return alloc_slot(std::move(entry));
    }

    // Allocate a slot holding an empty BehaviorAsset. Mutated via
    // NEUI_API_BEHAVIOR. Returns 0 on failure.
    uint32_t allocate_behavior()
    {
      auto entry = std::make_unique<MacOSAssetEntry>();
      entry->kind     = NEUI_ASSET_KIND_BEHAVIOR;
      entry->behavior = std::make_unique<neui_detail::BehaviorAsset>();
      return alloc_slot(std::move(entry));
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
      entry.reset();
      _free_slots.push_back(slot);
    }

    MacOSAssetEntry* get_slot(uint32_t slot)
    {
      if (slot == 0 || slot >= _handles.size()) return nullptr;
      return _handles[slot].get();
    }

    // Kind-polymorphic pixel readback for export paths (drag preview, future
    // file save, ...). Returns true and populates the out params for any
    // asset that resolves to displayable BGRA8 pixels - BITMAP today;
    // SURFACE will add one branch the day that kind lands. Returns false
    // for COMPOUND, BEHAVIOR, invalid slot, or any kind with no
    // presentable pixels. Pixels are width_px * height_px * 4 bytes.
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
        if (entry->pixels.empty()) return false;
        if (out_bgra)  *out_bgra  = entry->pixels.data();
        if (out_w_px)  *out_w_px  = entry->width_px;
        if (out_h_px)  *out_h_px  = entry->height_px;
        if (out_scale) *out_scale = entry->scale;
        return true;
      // case NEUI_ASSET_KIND_SURFACE: identical body once that kind lands
      //   (surface backing buffer is the same BGRA8 premul layout).
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
      _handles.clear();
      _free_slots.clear();
    }

  private:
    std::vector<std::unique_ptr<MacOSAssetEntry>> _handles;
    std::vector<uint32_t>                          _free_slots;

    uint32_t alloc_slot(std::unique_ptr<MacOSAssetEntry> entry)
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
        uint8_t* raw = neui_detail::load_image_bgra8_macos(p.c_str(), &w, &h);
        if (!raw) return false;
        neui_detail::free_image_bgra8(raw);
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

} // namespace macos_host
