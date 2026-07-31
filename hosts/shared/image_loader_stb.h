#pragma once

// stb_image-based file loader - the fallback decoder for platform layers with
// no OS imaging framework worth calling (Linux/X11, and the LVGL prototype host
// on Windows, which runs without COM). Decodes any stb-supported format
// (PNG / JPG / BMP / GIF / ...) into a heap-allocated BGRA8-premultiplied,
// top-down buffer matching what the cairo / d2d / cg / lvgl backends expect.
// The macOS / native-Windows counterparts are
// hosts/shared/macos/image_loader_macos.h and
// hosts/shared/win32/image_loader_win32.h (ImageIO / WIC).
//
// Platform-neutral by design - no OS guard - so every stb-based platform layer
// shares one copy of the premultiply + overflow-guard logic.
//
// stb_image's implementation must be emitted in exactly ONE translation unit:
// the includer defines STB_IMAGE_IMPLEMENTATION before including this header
// (platform_linux.cpp and platform_lvgl.cpp do, one per build). Every other
// includer gets declarations only.

#include <stb_image.h>

#include <cstdint>

namespace neui_detail
{
  // Decode `path` into a new[]-allocated BGRA8-premultiplied buffer. Caller
  // releases via free_image_bgra8_stb. Returns nullptr on failure.
  inline uint8_t* load_image_bgra8_stb(const char* path,
                                       uint32_t* width_out,
                                       uint32_t* height_out)
  {
    if (!path || !*path) return nullptr;
    int w = 0, h = 0, n = 0;
    unsigned char* rgba = stbi_load(path, &w, &h, &n, 4);  // force RGBA8
    if (!rgba) return nullptr;
    if (w <= 0 || h <= 0) { stbi_image_free(rgba); return nullptr; }

    // Guard the BGRA byte-count multiply against size_t overflow before we
    // new[] it. (stbi_load would already have failed to allocate an absurd
    // image, but make the bound explicit rather than rely on that.)
    size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
    if (count == 0 || count > (SIZE_MAX / 4)) { stbi_image_free(rgba); return nullptr; }
    uint8_t* out = new uint8_t[count * 4];
    for (size_t i = 0; i < count; ++i) {
      uint32_t r = rgba[i * 4 + 0];
      uint32_t g = rgba[i * 4 + 1];
      uint32_t b = rgba[i * 4 + 2];
      uint32_t a = rgba[i * 4 + 3];
      // RGBA (straight) -> BGRA premultiplied (round-to-nearest).
      out[i * 4 + 0] = static_cast<uint8_t>((b * a + 127) / 255);
      out[i * 4 + 1] = static_cast<uint8_t>((g * a + 127) / 255);
      out[i * 4 + 2] = static_cast<uint8_t>((r * a + 127) / 255);
      out[i * 4 + 3] = static_cast<uint8_t>(a);
    }
    stbi_image_free(rgba);

    if (width_out)  *width_out  = static_cast<uint32_t>(w);
    if (height_out) *height_out = static_cast<uint32_t>(h);
    return out;
  }

  inline void free_image_bgra8_stb(uint8_t* pixels) { delete[] pixels; }

} // namespace neui_detail
