#pragma once

#include "../shared/asset_store.h"
#include "../shared/win32/image_loader_win32.h"

namespace win32_host
{
  // Session-scoped asset table backing the public neui_asset_api_t.
  // Storage tier, slot allocation, surface paint coordination and context
  // lifecycle live in the shared neui_detail::AssetStore
  // (hosts/shared/asset_store.h); the only win32-specific piece is the
  // WIC image-loader policy below. The W32* aliases keep existing call
  // sites (widgets.cpp's compound storage lookups, cache walks) compiling
  // unchanged.
  struct W32ImageLoader
  {
    static uint8_t* load(const char* path, uint32_t* w_px, uint32_t* h_px)
    { return neui_detail::load_image_bgra8_w32(path, w_px, h_px); }
    static void free_pixels(uint8_t* p) { delete[] p; }
  };

  using W32CtxBitmap    = neui_detail::CtxBitmap;
  using W32AssetEntry   = neui_detail::AssetEntry;
  using W32AssetManager = neui_detail::AssetStore<W32ImageLoader>;

} // namespace win32_host
