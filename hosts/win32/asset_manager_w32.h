#pragma once

#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <neui/d/renderer.h>
#include <neui/d/assets.h>
#include "../shared/win32/image_loader_win32.h"

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

    W32AssetEntry* get_slot(uint32_t slot)
    {
      if (slot == 0 || slot >= _handles.size()) return nullptr;
      return _handles[slot].get();
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
