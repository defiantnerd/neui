#include "asset_manager.h"
#include "platform.h"

namespace neui_detail
{
  void AssetManager::split_ext(const std::string& name,
                                std::string& base_out, std::string& ext_out)
  {
    auto dot = name.rfind('.');
    if (dot == std::string::npos) {
      base_out = name;
      ext_out  = {};
    } else {
      base_out = name.substr(0, dot);
      ext_out  = name.substr(dot);
    }
  }

  bool AssetManager::load_pixels(const std::string& path, AssetEntry& entry)
  {
    uint32_t w = 0, h = 0;
    uint8_t* raw = xpl_host::platform_load_image(path.c_str(), &w, &h);
    if (!raw) return false;

    entry.width_px  = w;
    entry.height_px = h;
    entry.pixels.assign(raw, raw + static_cast<size_t>(w) * h * 4);
    xpl_host::platform_free_image(raw);
    return true;
  }

  std::string AssetManager::resolve_path(const std::string& name, float scale)
  {
    std::string base, ext;
    split_ext(name, base, ext);

    // Build the list of candidate paths to try, in preference order.
    // Each candidate is tried by attempting to load pixels; the first that succeeds wins.
    auto try_path = [&](const std::string& suffix) -> std::string {
      return base + suffix + ext;
    };

    // Preferred candidates by scale, then a higher-res fallback so a
    // deployment that ships only @2x (or @3x) still loads on a 96-DPI
    // display - the bitmap is downscaled at draw time. The fallback
    // entries are no-ops at higher scales because the same paths already
    // appear earlier in the list and the loop returns on the first hit.
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
      uint8_t* raw = xpl_host::platform_load_image(p.c_str(), &w, &h);
      if (raw) {
        xpl_host::platform_free_image(raw);
        return p;
      }
    }
    return {};
  }

  void* AssetManager::get_bitmap(const std::string& name, float scale,
                                  neui_render_backend_t* backend, neui_render_ctx_t ctx)
  {
    if (name.empty() || !backend || !ctx || !backend->create_bitmap) return nullptr;

    // Find or resolve and load the entry.
    std::string resolved = resolve_path(name, scale);
    if (resolved.empty()) return nullptr;

    auto it = _cache.find(resolved);
    if (it == _cache.end()) {
      AssetEntry entry;

      // Determine the actual scale of the resolved path.
      std::string base, ext;
      split_ext(name, base, ext);
      if (resolved == base + "@3x" + ext)      entry.scale = 3.0f;
      else if (resolved == base + "@2x" + ext) entry.scale = 2.0f;
      else                                      entry.scale = 1.0f;

      if (!load_pixels(resolved, entry)) return nullptr;
      it = _cache.emplace(resolved, std::move(entry)).first;
    }

    AssetEntry& e = it->second;

    // Find or create the GPU bitmap for this render context.
    auto bmp_it = e.bitmaps.find(ctx);
    if (bmp_it == e.bitmaps.end()) {
      void* bmp = backend->create_bitmap(ctx,
                                          e.width_px, e.height_px,
                                          e.pixels.data(),
                                          e.scale);
      if (!bmp) return nullptr;
      bmp_it = e.bitmaps.emplace(ctx, bmp).first;
    }
    return bmp_it->second;
  }

  bool AssetManager::get_logical_size(const std::string& name, float scale,
                                       float* width_out, float* height_out)
  {
    if (name.empty()) return false;
    std::string resolved = resolve_path(name, scale);
    if (resolved.empty()) return false;

    auto it = _cache.find(resolved);
    if (it == _cache.end()) {
      AssetEntry entry;
      std::string base, ext;
      split_ext(name, base, ext);
      if (resolved == base + "@3x" + ext)      entry.scale = 3.0f;
      else if (resolved == base + "@2x" + ext) entry.scale = 2.0f;
      else                                      entry.scale = 1.0f;
      if (!load_pixels(resolved, entry)) return false;
      it = _cache.emplace(resolved, std::move(entry)).first;
    }

    const AssetEntry& e = it->second;
    if (e.scale <= 0.0f) return false;
    if (width_out)  *width_out  = static_cast<float>(e.width_px)  / e.scale;
    if (height_out) *height_out = static_cast<float>(e.height_px) / e.scale;
    return true;
  }

  void AssetManager::release_context(neui_render_ctx_t ctx, neui_render_backend_t* backend)
  {
    if (!ctx || !backend || !backend->destroy_bitmap) return;
    for (auto& [path, entry] : _cache) {
      auto it = entry.bitmaps.find(ctx);
      if (it != entry.bitmaps.end()) {
        backend->destroy_bitmap(ctx, it->second);
        entry.bitmaps.erase(it);
      }
    }
  }

  void AssetManager::clear(neui_render_backend_t* backend)
  {
    if (backend && backend->destroy_bitmap) {
      for (auto& [path, entry] : _cache) {
        for (auto& [ctx, bmp] : entry.bitmaps)
          backend->destroy_bitmap(ctx, bmp);
      }
    }
    _cache.clear();
  }

} // namespace neui_detail
