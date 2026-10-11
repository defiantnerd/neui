// Scaling a coarse shader image up (nsl.h, upscale). Integer arithmetic only,
// so it is the same everywhere; it lives here to be optimised in every build
// type, since a layer below resolution 1 runs it on every render.

#include <algorithm>

#include "nsl.h"

namespace neui {
namespace nsl {

namespace {

// One axis: the px from `first` whose centres lie on texels [a, a + n) of a
// grid r / 65536 times as dense, and for each the two texels it filters
// (clamped to the edge) and the second one's weight in 1/256.
struct Axis {
  int first = 0;
  std::vector<int32_t> t0, t1;
  std::vector<uint32_t> f;
};

bool axis(int a, int n, uint32_t r, Axis& ax) {
  // Positions in 1/131072 texel: px d has its centre at r * (2d + 1).
  const int64_t R = r, one = int64_t(1) << 17;
  auto floor_div = [](int64_t v, int64_t m) { return v >= 0 ? v / m : -((m - 1 - v) / m); };
  auto first_at = [&](int64_t edge) {  // the first px whose centre is at or past `edge`
    int64_t d = floor_div(edge, 2 * R) - 1;
    while (R * (2 * d + 1) < edge) ++d;
    return d;
  };
  const int64_t d0 = first_at(int64_t(a) * one), d1 = first_at((int64_t(a) + n) * one);
  if (d1 - d0 > k_max_bitmap_side) return false;
  const size_t count = static_cast<size_t>(d1 - d0);
  ax.first = static_cast<int>(d0);
  ax.t0.resize(count);
  ax.t1.resize(count);
  ax.f.resize(count);
  for (size_t i = 0; i < count; ++i) {
    // texture2D's arithmetic: half a texel back, the weight rounded to 1/256.
    const int64_t s = R * (2 * (d0 + int64_t(i)) + 1) - int64_t(a) * one - one / 2;
    const int64_t t = floor_div(s, one);
    ax.t0[i] = static_cast<int32_t>(std::clamp<int64_t>(t, 0, n - 1));
    ax.t1[i] = static_cast<int32_t>(std::clamp<int64_t>(t + 1, 0, n - 1));
    ax.f[i] = static_cast<uint32_t>((s - t * one + 256) >> 9);
  }
  return true;
}

}  // namespace

bool upscale(const uint8_t* src, int w, int h, int sx, int sy, uint32_t r,
             std::vector<uint8_t>& dst, int& dx, int& dy, int& dw, int& dh) {
  Axis ax, ay;
  if (!src || w <= 0 || h <= 0 || r == 0 || !axis(sx, w, r, ax) || !axis(sy, h, r, ay) ||
      ax.f.empty() || ay.f.empty())
    return false;
  dx = ax.first;
  dy = ay.first;
  dw = static_cast<int>(ax.f.size());
  dh = static_cast<int>(ay.f.size());
  const size_t n = static_cast<size_t>(dw) * 4u;
  dst.assign(n * static_cast<size_t>(dh), 0);

  // Texel rows filtered across, the last two kept: the next row of px needs
  // at most one more.
  std::vector<uint16_t> row[2] = { std::vector<uint16_t>(n), std::vector<uint16_t>(n) };
  int held[2] = { -1, -1 };
  auto across = [&](int t, int keep) -> const uint16_t* {
    for (int i = 0; i < 2; ++i)
      if (held[i] == t) return row[i].data();
    const int i = held[0] == keep ? 1 : 0;
    const uint8_t* s = src + static_cast<size_t>(t) * static_cast<size_t>(w) * 4u;
    uint16_t* o = row[i].data();
    for (int x = 0; x < dw; ++x) {
      const uint8_t* p0 = s + static_cast<size_t>(ax.t0[x]) * 4u;
      const uint8_t* p1 = s + static_cast<size_t>(ax.t1[x]) * 4u;
      const uint32_t f = ax.f[x], g = 256u - f;
      for (int c = 0; c < 4; ++c) o[x * 4 + c] = static_cast<uint16_t>(p0[c] * g + p1[c] * f);
    }
    held[i] = t;
    return o;
  };
  for (int y = 0; y < dh; ++y) {
    const uint16_t* r0 = across(ay.t0[y], -1);
    const uint16_t* r1 = across(ay.t1[y], ay.t0[y]);
    const uint32_t f = ay.f[y], g = 256u - f;
    uint8_t* o = dst.data() + static_cast<size_t>(y) * n;
    for (size_t i = 0; i < n; ++i) o[i] = static_cast<uint8_t>((r0[i] * g + r1[i] * f + 32768u) >> 16);
  }
  return true;
}

}  // namespace nsl
}  // namespace neui
