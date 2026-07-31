#include "asset_manager.h"
#include "platform.h"

// Slot-table (handle-keyed) storage, allocation, surface paint and the
// shared context-lifecycle walk all live in hosts/shared/asset_store.h.
// This TU keeps only the xpl-specific path-keyed _cache tier backing the
// NEUI_W_IMAGE widget, plus the cache-aware lifecycle shadows.

namespace neui_detail
{
  // Decode the source `route` designates (a resolved @Nx file, or bytes from the
  // client resource provider) into `entry`.
  bool AssetManager::load_pixels(const ImageRoute& route, AssetEntry& entry)
  {
    uint32_t w = 0, h = 0;
    uint8_t* raw = decode_route(route, &w, &h);
    if (!raw || w == 0 || h == 0) {
      XplImageLoader::free_pixels(raw);
      return false;
    }

    entry.width_px  = w;
    entry.height_px = h;
    entry.pixels.assign(raw, raw + static_cast<size_t>(w) * h * 4);
    XplImageLoader::free_pixels(raw);
    return true;
  }

  void* AssetManager::get_bitmap(const std::string& name, float scale,
                                  neui_render_backend_t* backend, neui_render_ctx_t ctx)
  {
    if (name.empty() || !backend || !ctx || !backend->create_bitmap) return nullptr;

    // Find or resolve and load the entry. image_route is CACHED (misses too),
    // which matters here above all: this runs once per IMAGE widget per frame,
    // and resolution used to probe the @Nx ladder by decoding each candidate -
    // a full image decode every frame purely to answer "which variant?".
    const ImageRoute& route = image_route(name, scale);
    if (!route.found) return nullptr;

    auto it = _cache.find(route.cache_key);
    if (it == _cache.end()) {
      AssetEntry entry;
      entry.scale = route.scale;
      if (!load_pixels(route, entry)) return nullptr;
      it = _cache.emplace(route.cache_key, std::move(entry)).first;
    }

    AssetEntry& e = it->second;

    // Find or create the GPU bitmap for this render context. If the
    // backend has had to recreate its device-dependent state since we
    // cached the handle (D2D's D2DERR_RECREATE_TARGET path), the cached
    // pointer is dangling - drop it and re-upload against the new target.
    const uint32_t gen = backend->get_context_generation
      ? backend->get_context_generation(ctx) : 0u;
    auto bmp_it = e.bitmaps.find(ctx);
    if (bmp_it != e.bitmaps.end() && bmp_it->second.generation != gen) {
      if (backend->destroy_bitmap && bmp_it->second.bmp)
        backend->destroy_bitmap(ctx, bmp_it->second.bmp);
      e.bitmaps.erase(bmp_it);
      bmp_it = e.bitmaps.end();
    }
    if (bmp_it == e.bitmaps.end()) {
      void* bmp = backend->create_bitmap(ctx,
                                          e.width_px, e.height_px,
                                          e.pixels.data(),
                                          e.scale);
      if (!bmp) return nullptr;
      bmp_it = e.bitmaps.emplace(ctx, CtxBitmap{ bmp, gen }).first;
    }
    return bmp_it->second.bmp;
  }

  bool AssetManager::get_logical_size(const std::string& name, float scale,
                                       float* width_out, float* height_out)
  {
    if (name.empty()) return false;
    const ImageRoute& route = image_route(name, scale);
    if (!route.found) return false;

    auto it = _cache.find(route.cache_key);
    if (it == _cache.end()) {
      AssetEntry entry;
      entry.scale = route.scale;
      if (!load_pixels(route, entry)) return false;
      it = _cache.emplace(route.cache_key, std::move(entry)).first;
    }

    const AssetEntry& e = it->second;
    if (e.scale <= 0.0f) return false;
    if (width_out)  *width_out  = static_cast<float>(e.width_px)  / e.scale;
    if (height_out) *height_out = static_cast<float>(e.height_px) / e.scale;
    return true;
  }

  // --- Context lifecycle --------------------------------------------------

  void AssetManager::release_context(neui_render_ctx_t ctx, neui_render_backend_t* backend)
  {
    if (!ctx || !backend || !backend->destroy_bitmap) return;
    for (auto& [path, entry] : _cache) {
      auto it = entry.bitmaps.find(ctx);
      if (it != entry.bitmaps.end()) {
        if (it->second.bmp) backend->destroy_bitmap(ctx, it->second.bmp);
        entry.bitmaps.erase(it);
      }
    }
    AssetStore<XplImageLoader>::release_context(ctx, backend);
  }

  void AssetManager::clear(neui_render_backend_t* backend)
  {
    if (backend && backend->destroy_bitmap) {
      for (auto& [path, entry] : _cache) {
        for (auto& [ctx, cached] : entry.bitmaps)
          if (cached.bmp) backend->destroy_bitmap(ctx, cached.bmp);
      }
    }
    _cache.clear();
    AssetStore<XplImageLoader>::clear(backend);
  }

} // namespace neui_detail
