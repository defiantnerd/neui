#pragma once

#include "../shared/asset_store.h"
#include "../shared/ios/image_loader_ios.h"

namespace ios_host
{
  // Session-scoped asset table backing the public neui_asset_api_t. Direct
  // mirror of hosts/macos/asset_manager_macos.h: storage tier, slot
  // allocation, surface paint coordination and context lifecycle all live in
  // the shared neui_detail::AssetStore (hosts/shared/asset_store.h); the only
  // iOS-specific piece is the ImageIO (CGImageSource) loader policy below,
  // which forwards to the shared hosts/shared/ios/image_loader_ios.h decoder
  // (same BGRA8-premul normalisation + iOS-bundle resolution as the xpl iOS
  // platform layer).
  //
  // CG bitmaps are device-independent (no device-loss path), so the CtxBitmap
  // generation field is constant here - kept for symmetry with the win32 D2D
  // path so the painter draw_asset thunk shape stays identical across hosts.
  struct IOSImageLoader
  {
    static uint8_t* load(const char* path, uint32_t* w_px, uint32_t* h_px)
    { return neui_detail::load_image_bgra8_ios(path, w_px, h_px); }
    static void free_pixels(uint8_t* p) { neui_detail::free_image_bgra8(p); }
  };

  using IOSCtxBitmap    = neui_detail::CtxBitmap;
  using IOSAssetEntry   = neui_detail::AssetEntry;
  using IOSAssetManager = neui_detail::AssetStore<IOSImageLoader>;

} // namespace ios_host
