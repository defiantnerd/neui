#pragma once
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>
#include <neui/d/renderer.h>
#include <neui/d/assets.h>
#include <neui/d/painter.h>

#include "compound.h"
#include "behavior.h"
#include "painter.h"  // shared/painter.h - draw_asset_thunk_t + neui_painter

namespace neui_detail
{
  // A single loaded asset: kind discriminator, CPU pixels (for bitmap
  // kinds) and per-render-context GPU bitmap cache. Future kinds (SVG /
  // vector) will reuse the slot table but populate a different storage
  // tier instead of `pixels`.
  //
  // CtxBitmap pairs the cached GPU handle with the backend's per-ctx
  // generation counter at the moment of upload. On lookup we compare it
  // against the backend's current generation: if the backend has had to
  // recreate the underlying device (D2DERR_RECREATE_TARGET), the cached
  // handle is dangling for draw purposes and we re-upload against the
  // new target. Backends without device-loss (CG, null) return a
  // constant so the check is free.
  struct CtxBitmap
  {
    void*    bmp        = nullptr;
    uint32_t generation = 0;
  };

  struct AssetEntry
  {
    neui_asset_kind_t    kind        = NEUI_ASSET_KIND_BITMAP;
    uint32_t             width_px    = 0;   // physical pixel dimensions (bitmap only)
    uint32_t             height_px   = 0;
    float                scale       = 1.0f; // HiDPI factor loaded (1, 2, or 3)
    std::vector<uint8_t> pixels;             // BGRA8 premultiplied, width_px*height_px*4

    // Backend bitmap cache: one GPU resource per render context.
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
  };

  // Internal asset manager - lifetime tied to a Session.
  // Holds two storage tiers:
  //   * _cache (legacy, path-keyed) - used by the framework's NEUI_W_IMAGE
  //     widget so multiple IMAGE widgets pointing at the same file share
  //     CPU pixels and a per-ctx GPU upload.
  //   * _handles (handle-keyed) - backs the public neui_asset_api_t.
  //     Slot-reused vector; handle = (session_id << 16) | slot, slot 0
  //     unused (matches asset_none = UINT32_MAX as "invalid").
  // Thread-safety: not required (single-threaded UI).
  class AssetManager
  {
  public:
    // --- Path-keyed lookups (host-internal, used by NEUI_W_IMAGE) ---------

    void* get_bitmap(const std::string& name, float scale,
                     neui_render_backend_t* backend, neui_render_ctx_t ctx);

    bool get_logical_size(const std::string& name, float scale,
                          float* width_out, float* height_out);

    // --- Handle-keyed lookups (backs neui_asset_api_t) --------------------

    // Allocate a new asset slot with raw BGRA8 (premultiplied) pixels.
    // Returns 0 on failure. The returned value is the slot index (1..N);
    // callers pack it with the owning session id to form neui_asset_t::id.
    uint32_t allocate_bitmap(uint32_t width_px, uint32_t height_px,
                              const uint8_t* bgra_premul, float scale);

    // Allocate a new asset slot from a path / resource name. Resolves
    // @2x / @3x variants via resolve_path. Returns 0 on failure.
    uint32_t allocate_from_file(const std::string& name, float scale);

    // Allocate a new asset slot holding an empty CompoundAsset.
    // Mutated through NEUI_API_COMPOUND. Returns 0 on failure.
    uint32_t allocate_compound();

    // Allocate a new asset slot holding an empty BehaviorAsset.
    // Mutated through NEUI_API_BEHAVIOR. Returns 0 on failure.
    uint32_t allocate_behavior();

    // Allocate a new SURFACE asset slot. Creates an off-screen render
    // context via backend->create_offscreen_context and reserves the
    // matching BGRA8 pixel buffer (zero-filled until the first paint).
    // Returns 0 on backends without off-screen support (null) or on
    // allocation failure.
    uint32_t allocate_surface(uint32_t width_px, uint32_t height_px,
                               float scale,
                               neui_render_backend_t* backend);

    // Drive a client paint callback against a SURFACE entry's
    // off-screen ctx. The painter handed to the callback uses the same
    // host_token + draw_asset_thunk that the host installs in WIDGET_PAINT
    // dispatch, so nested draw_asset calls work transparently. On return,
    // the surface's CPU pixel buffer holds the freshly rendered frame
    // and every cached per-window GPU upload of this asset has been
    // dropped so subsequent draw_asset calls re-upload. No-op on
    // non-SURFACE slots, on null fn, or on backends without off-screen
    // support.
    void paint_surface(uint32_t slot,
                       uint32_t clear_argb,
                       neui_surface_paint_fn fn,
                       void* user,
                       neui_render_backend_t* backend,
                       void* host_token,
                       neui_detail::draw_asset_thunk_t draw_asset_thunk);

    // Release the slot. CPU pixels freed immediately; GPU caches dropped.
    void release_slot(uint32_t slot, neui_render_backend_t* backend);

    // Look up an asset by slot. Returns nullptr if the slot is unused or
    // out of range. Caller must not retain the pointer past the next
    // mutating call.
    AssetEntry* get_slot(uint32_t slot);

    // Kind-polymorphic pixel readback for export paths (drag preview,
    // future file save, ...). Returns true and populates the out params
    // for any asset that resolves to displayable BGRA8 pixels - BITMAP
    // today; SURFACE will add one branch the day that kind lands.
    // Returns false for COMPOUND, BEHAVIOR, invalid slot, or any kind
    // with no presentable pixels. Pixels are width_px * height_px * 4
    // bytes; pointer is borrowed (valid until the next mutating call).
    bool get_pixels_for_export(uint32_t slot,
                                const uint8_t** out_bgra,
                                uint32_t* out_w_px,
                                uint32_t* out_h_px,
                                float*    out_scale) const;

    // --- Context lifecycle ------------------------------------------------

    void release_context(neui_render_ctx_t ctx, neui_render_backend_t* backend);

    void clear(neui_render_backend_t* backend);

  private:
    // Cache keyed by resolved file path so different scale lookups that resolve to
    // the same file share a single AssetEntry.
    std::unordered_map<std::string, AssetEntry> _cache;

    // Slot table for handle-based assets (public neui_asset_api_t).
    // _handles[0] is intentionally unused so slot 0 maps to "invalid".
    std::vector<std::unique_ptr<AssetEntry>> _handles;
    std::vector<uint32_t>                    _free_slots;

    // Resolves the best available path for the requested scale.
    // Returns empty string if no suitable file is found.
    // Scale resolution:
    //   scale > 2.0 -> try @3x -> try @2x -> try base
    //   scale > 1.0 -> try @2x -> try base
    //   scale == 1.0 -> try base
    static std::string resolve_path(const std::string& name, float scale);

    // Splits "dir/file.ext" into {"dir/file", ".ext"}.
    static void split_ext(const std::string& name,
                           std::string& base_out, std::string& ext_out);

    // Attempts to load pixels from path. Returns true and populates entry on success.
    static bool load_pixels(const std::string& path, AssetEntry& entry);
  };

} // namespace neui_detail
