#pragma once
#include <string>
#include <unordered_map>

#include "../shared/asset_store.h"

namespace xpl_host
{
  // Platform image-loading seam (platform.h declares these; per-OS impls
  // in platform_{win32.cpp,macos.mm,null.cpp}). Redeclared here so the
  // loader policy below doesn't need the full platform.h.
  uint8_t* platform_load_image(const char* path,
                                uint32_t* width_out, uint32_t* height_out);
  uint8_t* platform_load_image_bytes(const uint8_t* data, size_t len,
                                      uint32_t* width_out, uint32_t* height_out);
  void platform_free_image(uint8_t* pixels);
}

namespace neui_detail
{
  // Loader policy routing the shared AssetStore through the xpl host's
  // platform image seam.
  struct XplImageLoader
  {
    static uint8_t* load(const char* path, uint32_t* w_px, uint32_t* h_px)
    { return xpl_host::platform_load_image(path, w_px, h_px); }
    static uint8_t* load_memory(const uint8_t* data, size_t len,
                                uint32_t* w_px, uint32_t* h_px)
    { return xpl_host::platform_load_image_bytes(data, len, w_px, h_px); }
    static void free_pixels(uint8_t* p) { xpl_host::platform_free_image(p); }
  };

  // Internal asset manager - lifetime tied to a Session.
  // The handle-keyed slot table (backing the public neui_asset_api_t)
  // lives in the shared AssetStore base (hosts/shared/asset_store.h).
  // This class adds the xpl-only path-keyed tier:
  //   * _cache (legacy, path-keyed) - used by the framework's NEUI_W_IMAGE
  //     widget so multiple IMAGE widgets pointing at the same file share
  //     CPU pixels and a per-ctx GPU upload.
  // Thread-safety: not required (single-threaded UI).
  class AssetManager : public AssetStore<XplImageLoader>
  {
  public:
    // --- Path-keyed lookups (host-internal, used by NEUI_W_IMAGE) ---------

    void* get_bitmap(const std::string& name, float scale,
                     neui_render_backend_t* backend, neui_render_ctx_t ctx);

    bool get_logical_size(const std::string& name, float scale,
                          float* width_out, float* height_out);

    // --- Context lifecycle ------------------------------------------------
    // Shadow the base versions: the path-keyed _cache entries carry their
    // own per-ctx GPU uploads that must be dropped alongside the slot
    // table's. Both call through to the AssetStore body for _handles.

    void release_context(neui_render_ctx_t ctx, neui_render_backend_t* backend);

    void clear(neui_render_backend_t* backend);

  private:
    // Cache keyed by ImageRoute::cache_key - the resolved @Nx file path, or a
    // per-scale-band synthetic key for client-provided bytes - so different
    // scale lookups that resolve to the same source share a single AssetEntry,
    // and ones that resolve to DIFFERENT pixels never collide.
    std::unordered_map<std::string, AssetEntry> _cache;

    // Decodes the source `route` designates into `entry`. Returns true on
    // success.
    bool load_pixels(const ImageRoute& route, AssetEntry& entry);
  };

} // namespace neui_detail
