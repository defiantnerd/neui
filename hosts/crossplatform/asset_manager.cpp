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

  // --- Handle-based asset allocation --------------------------------------

  uint32_t AssetManager::allocate_bitmap(uint32_t width_px, uint32_t height_px,
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

    // Slot 0 is intentionally unused so a returned 0 means "failure".
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

  uint32_t AssetManager::allocate_from_file(const std::string& name, float scale)
  {
    if (name.empty()) return 0;
    if (scale <= 0.0f) scale = 1.0f;

    std::string resolved = resolve_path(name, scale);
    if (resolved.empty()) return 0;

    AssetEntry tmp;
    std::string base, ext;
    split_ext(name, base, ext);
    if      (resolved == base + "@3x" + ext) tmp.scale = 3.0f;
    else if (resolved == base + "@2x" + ext) tmp.scale = 2.0f;
    else                                      tmp.scale = 1.0f;

    if (!load_pixels(resolved, tmp)) return 0;
    tmp.kind = NEUI_ASSET_KIND_BITMAP;

    auto entry = std::make_unique<AssetEntry>(std::move(tmp));

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

  uint32_t AssetManager::allocate_compound()
  {
    auto entry = std::make_unique<AssetEntry>();
    entry->kind     = NEUI_ASSET_KIND_COMPOUND;
    entry->compound = std::make_unique<CompoundAsset>();

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

  uint32_t AssetManager::allocate_behavior()
  {
    auto entry = std::make_unique<AssetEntry>();
    entry->kind     = NEUI_ASSET_KIND_BEHAVIOR;
    entry->behavior = std::make_unique<BehaviorAsset>();

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

  uint32_t AssetManager::allocate_surface(uint32_t width_px, uint32_t height_px,
                                            float scale,
                                            neui_render_backend_t* backend)
  {
    if (width_px == 0 || height_px == 0 || !backend
     || !backend->create_offscreen_context)
      return 0;
    if (scale <= 0.0f) scale = 1.0f;

    neui_render_ctx_t ctx = backend->create_offscreen_context(width_px, height_px, scale);
    if (!ctx) return 0;  // null backend / allocation failure

    auto entry = std::make_unique<AssetEntry>();
    entry->kind        = NEUI_ASSET_KIND_SURFACE;
    entry->width_px    = width_px;
    entry->height_px   = height_px;
    entry->scale       = scale;
    entry->surface_ctx = ctx;
    // Zero-fill the CPU buffer so the first draw_asset before any
    // paint_surface sees transparent black rather than uninitialised memory.
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

  void AssetManager::paint_surface(uint32_t slot,
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
    fn(&painter, &neui_detail::k_painter_api,
       static_cast<float>(entry->width_px)  / entry->scale,
       static_cast<float>(entry->height_px) / entry->scale,
       user);

    if (backend->pop_clip)  backend->pop_clip(ctx);
    if (backend->end_frame) backend->end_frame(ctx);

    // Pull the freshly rendered pixels into our CPU buffer.
    if (backend->read_pixels_bgra)
      backend->read_pixels_bgra(ctx, entry->pixels.data());

    // Drop every cached per-window GPU upload of this surface so the next
    // draw_asset re-uploads from the new pixel buffer. The cache key is
    // (window-ctx), not surface_ctx - destroy_bitmap runs against the
    // window ctx that owns each cached bitmap.
    if (backend->destroy_bitmap) {
      for (auto& [other_ctx, cached] : entry->bitmaps)
        if (cached.bmp) backend->destroy_bitmap(other_ctx, cached.bmp);
    }
    entry->bitmaps.clear();
    // Tinted variants reference the pre-repaint pixels and are stale; the
    // next tinted draw uploads against the freshly read-back buffer.
    neui_detail::release_all_tinted_bitmaps(entry.get(), backend);
  }

  void AssetManager::release_slot(uint32_t slot, neui_render_backend_t* backend)
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

  AssetEntry* AssetManager::get_slot(uint32_t slot)
  {
    if (slot == 0 || slot >= _handles.size()) return nullptr;
    return _handles[slot].get();
  }

  bool AssetManager::get_pixels_for_export(uint32_t slot,
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

  void AssetManager::release_context(neui_render_ctx_t ctx, neui_render_backend_t* backend)
  {
    if (!ctx || !backend || !backend->destroy_bitmap) return;
    for (auto& [path, entry] : _cache) {
      auto it = entry.bitmaps.find(ctx);
      if (it != entry.bitmaps.end()) {
        if (it->second.bmp) backend->destroy_bitmap(ctx, it->second.bmp);
        entry.bitmaps.erase(it);
      }
      // Path-keyed entries don't carry tinted bitmaps today (the NEUI_W_IMAGE
      // widget never tints), so there's nothing to release here.
    }
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

  void AssetManager::clear(neui_render_backend_t* backend)
  {
    if (backend && backend->destroy_bitmap) {
      for (auto& [path, entry] : _cache) {
        for (auto& [ctx, cached] : entry.bitmaps)
          if (cached.bmp) backend->destroy_bitmap(ctx, cached.bmp);
      }
      for (auto& entry : _handles) {
        if (!entry) continue;
        for (auto& [ctx, cached] : entry->bitmaps)
          if (cached.bmp) backend->destroy_bitmap(ctx, cached.bmp);
        neui_detail::release_all_tinted_bitmaps(entry.get(), backend);
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
    _cache.clear();
    _handles.clear();
    _free_slots.clear();
  }

} // namespace neui_detail
