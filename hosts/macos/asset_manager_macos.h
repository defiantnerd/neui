#pragma once

#include "../shared/asset_store.h"
#include "../shared/macos/image_loader_macos.h"

namespace macos_host
{
  // Session-scoped asset table backing the public neui_asset_api_t.
  // Storage tier, slot allocation, surface paint coordination and context
  // lifecycle live in the shared neui_detail::AssetStore
  // (hosts/shared/asset_store.h); the only macOS-specific piece is the
  // ImageIO loader policy below. The MacOS* aliases keep existing call
  // sites (widgets.mm's compound storage lookups, window.mm's painted
  // views) compiling unchanged.
  //
  // CG bitmaps are device-independent (no device-loss path), so the
  // CtxBitmap generation field is constant here - kept for symmetry with
  // the win32 D2D path so the painter draw_asset thunk shape stays
  // identical across hosts.
  struct MacOSImageLoader
  {
    static uint8_t* load(const char* path, uint32_t* w_px, uint32_t* h_px)
    { return neui_detail::load_image_bgra8_macos(path, w_px, h_px); }
    static uint8_t* load_memory(const uint8_t* data, size_t len,
                                uint32_t* w_px, uint32_t* h_px)
    { return neui_detail::load_image_bgra8_macos_memory(data, len, w_px, h_px); }
    static void free_pixels(uint8_t* p) { neui_detail::free_image_bgra8(p); }
  };

  using MacOSCtxBitmap    = neui_detail::CtxBitmap;
  using MacOSAssetEntry   = neui_detail::AssetEntry;
  using MacOSAssetManager = neui_detail::AssetStore<MacOSImageLoader>;

} // namespace macos_host
