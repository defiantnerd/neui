#pragma once

#if defined(__linux__) || (defined(__unix__) && !defined(__APPLE__))

// Linux image loader - mirror of hosts/shared/macos/image_loader_macos.h,
// using the vendored stb_image (third_party/stb/stb_image.h). Decodes any
// stb-supported format (PNG / JPG / BMP / GIF / ...) into a heap-allocated
// BGRA8-premultiplied, top-down buffer matching what the cairo / d2d / cg
// backends expect.
//
// stb_image's implementation must be emitted in exactly ONE translation unit:
// the includer defines STB_IMAGE_IMPLEMENTATION before including this header
// (platform_linux.cpp does). Every other includer gets declarations only.

#include <stb_image.h>

#include <cstdint>

namespace neui_detail
{
  // Decode `path` into a new[]-allocated BGRA8-premultiplied buffer. Caller
  // releases via free_image_bgra8_linux. Returns nullptr on failure.
  inline uint8_t* load_image_bgra8_linux(const char* path,
                                         uint32_t* width_out,
                                         uint32_t* height_out)
  {
    if (!path || !*path) return nullptr;
    int w = 0, h = 0, n = 0;
    unsigned char* rgba = stbi_load(path, &w, &h, &n, 4);  // force RGBA8
    if (!rgba) return nullptr;
    if (w <= 0 || h <= 0) { stbi_image_free(rgba); return nullptr; }

    size_t count = static_cast<size_t>(w) * static_cast<size_t>(h);
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

  inline void free_image_bgra8_linux(uint8_t* pixels) { delete[] pixels; }

} // namespace neui_detail

#endif // linux
