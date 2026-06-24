#include "neui_test.h"

#include "image_filter.h"  // hosts/shared - pure BGRA8 premultiplied filters

#include <cstdint>
#include <vector>

using namespace neui_test;
using neui_detail::image_gaussian_blur_bgra;

namespace
{
  // BGRA8 byte offsets within a pixel: [B, G, R, A].
  inline size_t px_idx(uint32_t x, uint32_t y, uint32_t w) { return (static_cast<size_t>(y) * w + x) * 4u; }

  inline void set_px(std::vector<uint8_t>& buf, uint32_t x, uint32_t y, uint32_t w,
                     uint8_t b, uint8_t g, uint8_t r, uint8_t a)
  {
    uint8_t* p = &buf[px_idx(x, y, w)];
    p[0] = b; p[1] = g; p[2] = r; p[3] = a;
  }

  inline uint8_t alpha_at(const std::vector<uint8_t>& buf, uint32_t x, uint32_t y, uint32_t w)
  {
    return buf[px_idx(x, y, w) + 3u];
  }
}

TEST_CASE("gaussian_blur: spreads a single opaque pixel symmetrically")
{
  const uint32_t w = 15, h = 15;
  std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4u, 0u);
  // Opaque white at the centre (premultiplied = same as straight at full alpha).
  set_px(buf, 7, 7, w, 255, 255, 255, 255);

  image_gaussian_blur_bgra(buf.data(), w, h, 1.5f, 1.5f);

  // Centre energy spread out, so its alpha dropped below the original 255.
  CHECK(alpha_at(buf, 7, 7, w) < 255);
  // Four-way neighbours are non-zero and symmetric.
  const uint8_t left  = alpha_at(buf, 6, 7, w);
  const uint8_t right = alpha_at(buf, 8, 7, w);
  const uint8_t up    = alpha_at(buf, 7, 6, w);
  const uint8_t down  = alpha_at(buf, 7, 8, w);
  CHECK(left > 0);
  CHECK_EQ(left, right);
  CHECK_EQ(up, down);
  CHECK_EQ(left, up);
  // Far corner is untouched by a small-sigma blur.
  CHECK_EQ(alpha_at(buf, 0, 0, w), static_cast<uint8_t>(0));
}

TEST_CASE("gaussian_blur: sigma <= 0 is a no-op")
{
  const uint32_t w = 8, h = 8;
  std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4u, 0u);
  set_px(buf, 4, 4, w, 10, 20, 30, 200);
  std::vector<uint8_t> before = buf;

  image_gaussian_blur_bgra(buf.data(), w, h, 0.0f, 0.0f);
  CHECK(buf == before);

  image_gaussian_blur_bgra(buf.data(), w, h, -3.0f, -3.0f);
  CHECK(buf == before);
}

TEST_CASE("gaussian_blur: anisotropic spreads only the requested axis")
{
  const uint32_t w = 15, h = 15;
  std::vector<uint8_t> buf(static_cast<size_t>(w) * h * 4u, 0u);
  set_px(buf, 7, 7, w, 255, 255, 255, 255);

  // Horizontal only (sigma_y == 0).
  image_gaussian_blur_bgra(buf.data(), w, h, 2.0f, 0.0f);

  CHECK(alpha_at(buf, 6, 7, w) > 0);   // horizontal neighbours got energy
  CHECK(alpha_at(buf, 8, 7, w) > 0);
  CHECK_EQ(alpha_at(buf, 7, 6, w), static_cast<uint8_t>(0));   // vertical untouched
  CHECK_EQ(alpha_at(buf, 7, 8, w), static_cast<uint8_t>(0));
}
