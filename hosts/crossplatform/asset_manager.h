#pragma once
#include <string>
#include <unordered_map>
#include <vector>
#include <neui/d/renderer.h>

namespace neui_detail
{
  // A single loaded image: raw BGRA8 pixels + per-render-context GPU bitmaps.
  struct AssetEntry
  {
    uint32_t             width_px    = 0;   // physical pixel dimensions
    uint32_t             height_px   = 0;
    float                scale       = 1.0f; // HiDPI factor loaded (1, 2, or 3)
    std::vector<uint8_t> pixels;             // BGRA8 premultiplied, width_px*height_px*4

    // Backend bitmap cache: one GPU resource per render context.
    std::unordered_map<neui_render_ctx_t, void*> bitmaps;
  };

  // Internal asset manager - lifetime tied to a Session.
  // Thread-safety: not required (single-threaded UI).
  class AssetManager
  {
  public:
    // Returns the backend bitmap for the named image at the given scale/context.
    // Loads from disk and creates the GPU resource on first access.
    // Returns nullptr if the image cannot be found or decoded.
    void* get_bitmap(const std::string& name, float scale,
                     neui_render_backend_t* backend, neui_render_ctx_t ctx);

    // Returns the image's intrinsic dimensions in logical (96-DPI) pixels,
    // i.e. physical_size / asset_scale. Loads the image if not yet cached.
    // Returns true on success, false if the image cannot be found / decoded.
    bool get_logical_size(const std::string& name, float scale,
                          float* width_out, float* height_out);

    // Release all GPU bitmaps associated with ctx (call before destroying the context).
    void release_context(neui_render_ctx_t ctx, neui_render_backend_t* backend);

    // Release all cached assets (call on session destroy).
    void clear(neui_render_backend_t* backend);

  private:
    // Cache keyed by resolved file path so different scale lookups that resolve to
    // the same file share a single AssetEntry.
    std::unordered_map<std::string, AssetEntry> _cache;

    // Resolves the best available path for the requested scale.
    // Returns empty string if no suitable file is found.
    // Scale resolution:
    //   scale > 2.0 → try @3x → try @2x → try base
    //   scale > 1.0 → try @2x → try base
    //   scale == 1.0 → try base
    static std::string resolve_path(const std::string& name, float scale);

    // Splits "dir/file.ext" into {"dir/file", ".ext"}.
    static void split_ext(const std::string& name,
                           std::string& base_out, std::string& ext_out);

    // Attempts to load pixels from path. Returns true and populates entry on success.
    static bool load_pixels(const std::string& path, AssetEntry& entry);
  };

} // namespace neui_detail
