#pragma once

// Portable CPU Gaussian blur over premultiplied BGRA8 pixel buffers.
//
// This is the feGaussianBlur primitive the SVG filter-graph engine
// (filter_graph.h) reuses, and also backs the surface_blur convenience op. It
// runs on the CPU pixel buffer a SURFACE reads back after paint_surface, so
// the result is pixel-identical on every backend (no D2D / CG / Cairo
// divergence) and needs no backend support. Surfaces are baked once (startup
// / theme / DPI change), not blurred per frame, so a CPU separable box blur
// is both fast enough and the simplest correct choice.
//
// Pixel layout: BGRA8, bytes [B, G, R, A] per pixel (0xAARRGGBB stored
// little-endian), colour channels PREMULTIPLIED by alpha. Blurring in
// premultiplied space is the correct convolution domain (it avoids the dark
// fringes a straight-alpha blur leaves at transparency edges).
//
// Header-only / inline, no neui types beyond <cstdint>; Tier-1 unit-tested in
// tests/test_image_filter.cpp.

#include <cstdint>
#include <cstring>
#include <cmath>
#include <vector>
#include <algorithm>

namespace neui_detail {

  inline int img_clampi(int v, int lo, int hi)
  {
    return v < lo ? lo : (v > hi ? hi : v);
  }

  // Box widths approximating a Gaussian of std-dev `sigma` with `n` passes
  // (Ivan Kutskir / Wojciech Jarosz "Fast Almost-Gaussian Filtering"). Each
  // returned size is an odd box DIAMETER; the per-pass radius is (size-1)/2.
  inline void img_boxes_for_gauss(float sigma, int n, int* sizes)
  {
    float w_ideal = std::sqrt((12.0f * sigma * sigma / static_cast<float>(n)) + 1.0f);
    int wl = static_cast<int>(std::floor(w_ideal));
    if ((wl & 1) == 0) wl -= 1;            // force odd
    if (wl < 1) wl = 1;
    int wu = wl + 2;
    float m_ideal = (12.0f * sigma * sigma
                     - static_cast<float>(n * wl * wl)
                     - static_cast<float>(4 * n * wl)
                     - static_cast<float>(3 * n))
                    / static_cast<float>(-4 * wl - 4);
    int m = static_cast<int>(std::lround(m_ideal));
    for (int i = 0; i < n; ++i)
      sizes[i] = (i < m) ? wl : wu;
  }

  // One horizontal box-blur pass of radius r, edge-clamped, 4 channels.
  inline void img_box_blur_h(const uint8_t* src, uint8_t* dst,
                             int w, int h, int r)
  {
    const size_t row_bytes = static_cast<size_t>(w) * 4u;
    if (r < 1) { std::memcpy(dst, src, row_bytes * static_cast<size_t>(h)); return; }
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    for (int y = 0; y < h; ++y) {
      const uint8_t* row  = src + static_cast<size_t>(y) * row_bytes;
      uint8_t*       orow = dst + static_cast<size_t>(y) * row_bytes;
      int acc[4] = { 0, 0, 0, 0 };
      for (int k = -r; k <= r; ++k) {
        const uint8_t* p = row + static_cast<size_t>(img_clampi(k, 0, w - 1)) * 4u;
        for (int c = 0; c < 4; ++c) acc[c] += p[c];
      }
      for (int x = 0; x < w; ++x) {
        uint8_t* o = orow + static_cast<size_t>(x) * 4u;
        for (int c = 0; c < 4; ++c)
          o[c] = static_cast<uint8_t>(static_cast<float>(acc[c]) * inv + 0.5f);
        const uint8_t* pin  = row + static_cast<size_t>(img_clampi(x + r + 1, 0, w - 1)) * 4u;
        const uint8_t* pout = row + static_cast<size_t>(img_clampi(x - r,     0, w - 1)) * 4u;
        for (int c = 0; c < 4; ++c) acc[c] += static_cast<int>(pin[c]) - static_cast<int>(pout[c]);
      }
    }
  }

  // One vertical box-blur pass of radius r, edge-clamped, 4 channels.
  inline void img_box_blur_v(const uint8_t* src, uint8_t* dst,
                             int w, int h, int r)
  {
    const size_t row_bytes = static_cast<size_t>(w) * 4u;
    if (r < 1) { std::memcpy(dst, src, row_bytes * static_cast<size_t>(h)); return; }
    const float inv = 1.0f / static_cast<float>(2 * r + 1);
    for (int x = 0; x < w; ++x) {
      const uint8_t* col = src + static_cast<size_t>(x) * 4u;
      int acc[4] = { 0, 0, 0, 0 };
      for (int k = -r; k <= r; ++k) {
        const uint8_t* p = col + static_cast<size_t>(img_clampi(k, 0, h - 1)) * row_bytes;
        for (int c = 0; c < 4; ++c) acc[c] += p[c];
      }
      for (int y = 0; y < h; ++y) {
        uint8_t* o = dst + static_cast<size_t>(y) * row_bytes + static_cast<size_t>(x) * 4u;
        for (int c = 0; c < 4; ++c)
          o[c] = static_cast<uint8_t>(static_cast<float>(acc[c]) * inv + 0.5f);
        const uint8_t* pin  = col + static_cast<size_t>(img_clampi(y + r + 1, 0, h - 1)) * row_bytes;
        const uint8_t* pout = col + static_cast<size_t>(img_clampi(y - r,     0, h - 1)) * row_bytes;
        for (int c = 0; c < 4; ++c) acc[c] += static_cast<int>(pin[c]) - static_cast<int>(pout[c]);
      }
    }
  }

  // Run `n` ping-pong box passes on one axis, leaving the result back in `px`.
  inline void img_blur_axis_n(uint8_t* px, uint8_t* tmp, int w, int h,
                              const int* boxes, int n, bool horizontal)
  {
    uint8_t* a = px;
    uint8_t* b = tmp;
    for (int i = 0; i < n; ++i) {
      const int r = (boxes[i] - 1) / 2;
      if (horizontal) img_box_blur_h(a, b, w, h, r);
      else            img_box_blur_v(a, b, w, h, r);
      std::swap(a, b);
    }
    if (a != px)
      std::memcpy(px, a, static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
  }

  // Gaussian blur (3-box approximation) of a premultiplied BGRA8 buffer, in
  // place. sigma_x / sigma_y are per-axis std-deviations in PIXELS; an axis
  // with sigma <= 0 is left unblurred.
  inline void image_gaussian_blur_bgra(uint8_t* px, uint32_t w, uint32_t h,
                                       float sigma_x, float sigma_y)
  {
    if (!px || w == 0 || h == 0) return;
    if (sigma_x <= 0.0f && sigma_y <= 0.0f) return;
    std::vector<uint8_t> tmp(static_cast<size_t>(w) * h * 4u);
    const int wi = static_cast<int>(w), hi = static_cast<int>(h);
    if (sigma_x > 0.0f) {
      int boxes[3];
      img_boxes_for_gauss(sigma_x, 3, boxes);
      img_blur_axis_n(px, tmp.data(), wi, hi, boxes, 3, /*horizontal*/ true);
    }
    if (sigma_y > 0.0f) {
      int boxes[3];
      img_boxes_for_gauss(sigma_y, 3, boxes);
      img_blur_axis_n(px, tmp.data(), wi, hi, boxes, 3, /*horizontal*/ false);
    }
  }

  // (feDropShadow is now expressed as a filter graph - see filter_graph.h and
  // AssetStore::drop_shadow_surface - so this file keeps only the Gaussian
  // blur primitive the engine reuses.)

} // namespace neui_detail
