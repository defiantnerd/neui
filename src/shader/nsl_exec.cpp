// .nsl CPU executor: evaluates a compiled program over the device pixels of
// a layer and packs premultiplied BGRA8. Pixel grid, coverage, rounding and
// dithering follow docs/shader-language.md section 11.
//
// execute() runs the pixel program 64 pixels at a time through explicit
// 4-wide SIMD kernels (SSE2 / NEON / portable). execute_scalar() runs the
// same program one pixel at a time through eval_op(); the two must produce
// the same bytes, and the unit tests check that they do.

#include "nsl_ir.h"

#include <algorithm>
#include <cstring>
#include <vector>

namespace neui {
namespace nsl {
namespace detail {
namespace {

constexpr int k_lanes = 64;

// The device pixels a draw covers, and the values the pixel grid needs.
struct Geometry {
  bool ok = false;
  int i0 = 0, j0 = 0, w = 0, h = 0;
  float X0 = 0, X1 = 0, Y0 = 0, Y1 = 0;  // device rect
  float fx = 0, fy = 0;                  // sub-pixel remainder of the origin
};

Geometry geometry(const Draw& d) {
  Geometry g;
  const float s = d.scale;
  const float lim = 8388608.0f;  // device coordinates stay exact integers below 2^23
  if (!(s > 0.0f) || !(d.w > 0.0f) || !(d.h > 0.0f)) return g;
  g.X0 = d.x * s;
  g.Y0 = d.y * s;
  g.X1 = g.X0 + d.w * s;
  g.Y1 = g.Y0 + d.h * s;
  if (!(m_abs(g.X0) < lim && m_abs(g.X1) < lim && m_abs(g.Y0) < lim && m_abs(g.Y1) < lim)) return g;
  const float fi0 = m_floor(g.X0), fj0 = m_floor(g.Y0);
  const float fi1 = -m_floor(-g.X1), fj1 = -m_floor(-g.Y1);  // ceil
  g.i0 = static_cast<int>(fi0);
  g.j0 = static_cast<int>(fj0);
  g.w = static_cast<int>(fi1) - g.i0;
  g.h = static_cast<int>(fj1) - g.j0;
  g.fx = g.X0 - fi0;
  g.fy = g.Y0 - fj0;
  g.ok = g.w > 0 && g.h > 0 && g.w <= k_max_bitmap_side && g.h <= k_max_bitmap_side;
  return g;
}

// Evaluates the draw program; returns its registers.
std::vector<float> run_draw(const Program& pg, const Draw& d) {
  std::vector<float> uni(static_cast<size_t>(pg.uniform_count));
  if (d.uniforms) std::copy(d.uniforms, d.uniforms + pg.uniform_count, uni.begin());
  else default_uniforms(pg, uni.data());
  std::vector<float> R(static_cast<size_t>(pg.draw_regs));
  for (const auto& c : pg.draw_consts) R[c.first] = c.second;
  for (const auto& u : pg.draw_uniforms) R[u.first] = uni[u.second];
  for (const auto& r : pg.draw_reserved) {
    float v = 0.0f;
    switch (r.second) {
      case RES_SIZE_X: v = d.w; break;
      case RES_SIZE_Y: v = d.h; break;
      case RES_SCALE: v = d.scale; break;
      case RES_PX: v = 1.0f / d.scale; break;
      default: v = d.time; break;
    }
    R[r.first] = v;
  }
  for (const Ins& in : pg.draw_code) R[in.d] = eval_op(in.op, R[in.a], R[in.b], R[in.c]);
  return R;
}

// The pixel pool for one draw: constants and draw-program values.
std::vector<float> make_pool(const Program& pg, const std::vector<float>& DR) {
  std::vector<float> pool(pg.pixel_pool.size());
  for (size_t i = 0; i < pool.size(); ++i) {
    const Program::PoolEntry& e = pg.pixel_pool[i];
    pool[i] = e.draw_reg >= 0 ? DR[static_cast<size_t>(e.draw_reg)] : e.value;
  }
  return pool;
}

// Runs pixel_code[begin, end).
void run_pixels_scalar(const Program& pg, size_t begin, size_t end, const float* pool, float* R) {
  for (size_t k = begin; k < end; ++k) {
    const Ins& in = pg.pixel_code[k];
    R[in.d] = in.op == OP_LOAD ? pool[in.a] : eval_op(in.op, R[in.a], R[in.b], R[in.c]);
  }
}

void run_pixels_simd(const Program& pg, size_t begin, size_t end, const float* pool, float* R) {
  constexpr int L = k_lanes;
  for (size_t k = begin; k < end; ++k) {
    const Ins& in = pg.pixel_code[k];
    float* d = R + static_cast<size_t>(in.d) * L;
    const float* a = R + static_cast<size_t>(in.a) * L;
    const float* b = R + static_cast<size_t>(in.b) * L;
    const float* c = R + static_cast<size_t>(in.c) * L;
#define NSL_K1(expr) for (int i = 0; i < L; i += 4) { const F4 x = f4_load(a + i); f4_store(d + i, expr); } break;
#define NSL_K2(expr) for (int i = 0; i < L; i += 4) { const F4 x = f4_load(a + i), y = f4_load(b + i); f4_store(d + i, expr); } break;
    switch (in.op) {
      case OP_ADD:   NSL_K2(x + y)
      case OP_SUB:   NSL_K2(x - y)
      case OP_MUL:   NSL_K2(x * y)
      case OP_DIV:   NSL_K2(x / y)
      case OP_NEG:   NSL_K1(f4_neg(x))
      case OP_MIN:   NSL_K2(f4_min(x, y))
      case OP_MAX:   NSL_K2(f4_max(x, y))
      case OP_ABS:   NSL_K1(f4_abs(x))
      case OP_FLOOR: NSL_K1(f4_floor(x))
      case OP_SQRT:  NSL_K1(f4_sqrt(x))
      case OP_SIN:   NSL_K1(f4_sin(x))
      case OP_COS:   NSL_K1(f4_cos(x))
      case OP_EXP2:  NSL_K1(f4_exp2(x))
      case OP_LOG2:  NSL_K1(f4_log2(x))
      case OP_ATAN2: NSL_K2(f4_atan2(x, y))
      case OP_HASH2: NSL_K2(f4_hash2(x, y))
      case OP_LT:    NSL_K2(f4_sel(f4_lt(x, y), f4(1.0f), f4(0.0f)))
      case OP_LE:    NSL_K2(f4_sel(f4_le(x, y), f4(1.0f), f4(0.0f)))
      case OP_EQ:    NSL_K2(f4_sel(f4_eq(x, y), f4(1.0f), f4(0.0f)))
      case OP_NE:    NSL_K2(f4_sel(f4_ne(x, y), f4(1.0f), f4(0.0f)))
      case OP_SEL:
        for (int i = 0; i < L; i += 4)
          f4_store(d + i, f4_sel(f4_ne(f4_load(a + i), f4(0.0f)), f4_load(b + i), f4_load(c + i)));
        break;
      case OP_LOAD: {
        const F4 v = f4(pool[in.a]);
        for (int i = 0; i < L; i += 4) f4_store(d + i, v);
        break;
      }
      default: break;
    }
#undef NSL_K1
#undef NSL_K2
  }
}

// Output rule, one pixel: clamp to [0, 1] (NaN -> 0), alpha times coverage,
// premultiply in float, alpha rounded half up, colour floored after adding
// the dither offset d, never above alpha.
inline float clamp01(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }

inline void pack1(float r, float g, float b, float a, float cov, float d, uint8_t* dst) {
  const float al = clamp01(a) * cov;
  const int32_t a8 = static_cast<int32_t>(al * 255.0f + 0.5f);
  const int32_t b8 = static_cast<int32_t>(clamp01(b) * al * 255.0f + d);
  const int32_t g8 = static_cast<int32_t>(clamp01(g) * al * 255.0f + d);
  const int32_t r8 = static_cast<int32_t>(clamp01(r) * al * 255.0f + d);
  dst[0] = static_cast<uint8_t>(std::min(b8, a8));
  dst[1] = static_cast<uint8_t>(std::min(g8, a8));
  dst[2] = static_cast<uint8_t>(std::min(r8, a8));
  dst[3] = static_cast<uint8_t>(a8);
}

// Four pixels at once: the same operations as pack1, in the same order, on
// the pixels at r[0..3] etc. with coverage cx[k] * cy and dither offsets
// d[0..3]. Writes 16 bytes (little-endian BGRA words).
#if NSL_X86 && !defined(NSL_FORCE_PORTABLE)
// Raw SSE2 rather than the F4 / I4 wrappers: nested this deeply, MSVC keeps
// every wrapper temporary on the stack (measured: 83 instructions per pixel,
// slower than pack1). The op kernels are shallow and do not suffer from it.
NSL_FI __m128 sse_clamp01(__m128 v, __m128 zero, __m128 one) {
  const __m128 lt1 = _mm_cmplt_ps(v, one);
  const __m128 inner = _mm_or_ps(_mm_and_ps(lt1, v), _mm_andnot_ps(lt1, one));
  return _mm_and_ps(_mm_cmpgt_ps(v, zero), inner);  // NaN -> 0
}
NSL_FI __m128i sse_channel(__m128 c, __m128 al, __m128 k255, __m128 d, __m128i a8, __m128 zero, __m128 one) {
  const __m128i c8 = _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(_mm_mul_ps(sse_clamp01(c, zero, one), al), k255), d));
  const __m128i lt = _mm_cmplt_epi32(c8, a8);  // min(c8, a8)
  return _mm_or_si128(_mm_and_si128(lt, c8), _mm_andnot_si128(lt, a8));
}
NSL_FI void pack4(const float* r, const float* g, const float* b, const float* a, const float* cx, float cy,
                  const float* d, uint8_t* dst) {
  const __m128 zero = _mm_setzero_ps(), one = _mm_set1_ps(1.0f), k255 = _mm_set1_ps(255.0f);
  const __m128 dv = _mm_loadu_ps(d);
  const __m128 al = _mm_mul_ps(sse_clamp01(_mm_loadu_ps(a), zero, one), _mm_mul_ps(_mm_loadu_ps(cx), _mm_set1_ps(cy)));
  const __m128i a8 = _mm_cvttps_epi32(_mm_add_ps(_mm_mul_ps(al, k255), _mm_set1_ps(0.5f)));
  const __m128i b8 = sse_channel(_mm_loadu_ps(b), al, k255, dv, a8, zero, one);
  const __m128i g8 = sse_channel(_mm_loadu_ps(g), al, k255, dv, a8, zero, one);
  const __m128i r8 = sse_channel(_mm_loadu_ps(r), al, k255, dv, a8, zero, one);
  const __m128i word = _mm_or_si128(_mm_or_si128(b8, _mm_slli_epi32(g8, 8)),
                                    _mm_or_si128(_mm_slli_epi32(r8, 16), _mm_slli_epi32(a8, 24)));
  _mm_storeu_si128(reinterpret_cast<__m128i*>(dst), word);
}
#else
NSL_FI F4 f4_clamp01(F4 v) {
  return f4_sel(f4_gt(v, f4(0.0f)), f4_sel(f4_lt(v, f4(1.0f)), v, f4(1.0f)), f4(0.0f));
}
NSL_FI I4 i4_min(I4 a, I4 b) { return i4_sel(i4_lt(a, b), a, b); }

NSL_FI void pack4(const float* r, const float* g, const float* b, const float* a, const float* cx, float cy,
                  const float* d, uint8_t* dst) {
  const F4 k255 = f4(255.0f), dv = f4_load(d);
  const F4 al = f4_clamp01(f4_load(a)) * (f4_load(cx) * f4(cy));
  const I4 a8 = f4_trunc(al * k255 + f4(0.5f));
  const I4 b8 = i4_min(f4_trunc(f4_clamp01(f4_load(b)) * al * k255 + dv), a8);
  const I4 g8 = i4_min(f4_trunc(f4_clamp01(f4_load(g)) * al * k255 + dv), a8);
  const I4 r8 = i4_min(f4_trunc(f4_clamp01(f4_load(r)) * al * k255 + dv), a8);
  i4_store(dst, b8 | i4_shl<8>(g8) | i4_shl<16>(r8) | i4_shl<24>(a8));
}
#endif

const float k_bayer4[16] = {0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5};

template <bool Simd>
bool run(const Program& pg, const Draw& d, uint8_t* out, size_t stride) {
  constexpr int L = Simd ? k_lanes : 1;
  FpEnvGuard fp;
  const Geometry g = geometry(d);
  if (!g.ok || !out) return false;
  const std::vector<float> pool = make_pool(pg, run_draw(pg, d));
  std::vector<float> regs(static_cast<size_t>(pg.pixel_regs) * L);
  float* R = regs.data();
  auto reg = [&](uint16_t r) { return R + static_cast<size_t>(r) * L; };
  const size_t pro = static_cast<size_t>(pg.pixel_prologue), all = pg.pixel_code.size();
  if constexpr (Simd) run_pixels_simd(pg, 0, pro, pool.data(), R);  // pinned values, once
  else run_pixels_scalar(pg, 0, pro, pool.data(), R);

  // p.x and the column coverage depend only on the column.
  const int w_pad = (g.w + L - 1) / L * L;
  std::vector<float> px_col(static_cast<size_t>(w_pad)), cx_col(static_cast<size_t>(w_pad));
  const float fi0 = static_cast<float>(g.i0), fj0 = static_cast<float>(g.j0);
  for (int i = 0; i < w_pad; ++i) {
    const float col = fi0 + static_cast<float>(i);
    px_col[static_cast<size_t>(i)] = ((static_cast<float>(i) + 0.5f) - g.fx) / d.scale;
    cx_col[static_cast<size_t>(i)] = m_min(col + 1.0f, g.X1) - m_max(col, g.X0);
  }
  const bool dither = (pg.flag_bits & NSL_DITHER) != 0;
  float* px = reg(pg.px);
  float* py = reg(pg.py);
  const float* out_r = reg(pg.out[0]);
  const float* out_g = reg(pg.out[1]);
  const float* out_b = reg(pg.out[2]);
  const float* out_a = reg(pg.out[3]);

  for (int y = 0; y < g.h; ++y) {
    const float pyv = ((static_cast<float>(y) + 0.5f) - g.fy) / d.scale;
    const float row = fj0 + static_cast<float>(y);
    const float cy = m_min(row + 1.0f, g.Y1) - m_max(row, g.Y0);
    float dv[4] = {0.5f, 0.5f, 0.5f, 0.5f};
    if (dither)
      for (int k = 0; k < 4; ++k) dv[k] = (k_bayer4[(y & 3) * 4 + k] + 0.5f) / 16.0f;
    uint8_t* dst_row = out + static_cast<size_t>(y) * stride;
    for (int x0 = 0; x0 < g.w; x0 += L) {
      std::memcpy(px, px_col.data() + x0, sizeof(float) * L);
      std::fill(py, py + L, pyv);
      if constexpr (Simd) run_pixels_simd(pg, pro, all, pool.data(), R);
      else run_pixels_scalar(pg, pro, all, pool.data(), R);
      const int n = std::min(L, g.w - x0);
      uint8_t* dst = dst_row + static_cast<size_t>(x0) * 4u;
      const float* cx = cx_col.data() + x0;
      int i = 0;
      if constexpr (Simd) {
        for (; i + 4 <= n; i += 4)
          pack4(out_r + i, out_g + i, out_b + i, out_a + i, cx + i, cy, dv, dst + static_cast<size_t>(i) * 4u);
      }
      for (; i < n; ++i)
        pack1(out_r[i], out_g[i], out_b[i], out_a[i], cx[i] * cy, dv[(x0 + i) & 3],
              dst + static_cast<size_t>(i) * 4u);
    }
  }
  return true;
}

template <bool Simd>
void eval_points(const Program& pg, const Draw& d, const float* xs, const float* ys, int n, float* out) {
  constexpr int L = Simd ? k_lanes : 1;
  FpEnvGuard fp;
  const std::vector<float> pool = make_pool(pg, run_draw(pg, d));
  std::vector<float> regs(static_cast<size_t>(pg.pixel_regs) * L);
  float* R = regs.data();
  auto reg = [&](uint16_t r) { return R + static_cast<size_t>(r) * L; };
  const size_t pro = static_cast<size_t>(pg.pixel_prologue), all = pg.pixel_code.size();
  if constexpr (Simd) run_pixels_simd(pg, 0, pro, pool.data(), R);
  else run_pixels_scalar(pg, 0, pro, pool.data(), R);
  for (int base = 0; base < n; base += L) {
    const int m = std::min(L, n - base);
    float* px = reg(pg.px);
    float* py = reg(pg.py);
    for (int i = 0; i < L; ++i) {
      px[i] = i < m ? xs[base + i] : 0.0f;
      py[i] = i < m ? ys[base + i] : 0.0f;
    }
    if constexpr (Simd) run_pixels_simd(pg, pro, all, pool.data(), R);
    else run_pixels_scalar(pg, pro, all, pool.data(), R);
    for (int i = 0; i < m; ++i)
      for (int c = 0; c < 4; ++c) out[static_cast<size_t>(base + i) * 4u + static_cast<size_t>(c)] = reg(pg.out[c])[i];
  }
}

}  // namespace
}  // namespace detail

void evaluate(const Program& p, const Draw& d, const float* xs, const float* ys, int n, float* out,
              bool simd) {
  if (simd) detail::eval_points<true>(p, d, xs, ys, n, out);
  else detail::eval_points<false>(p, d, xs, ys, n, out);
}

Placement placement(const Draw& d) {
  const detail::Geometry g = detail::geometry(d);
  Placement p;
  if (g.ok) {
    p.x = g.i0;
    p.y = g.j0;
    p.w = g.w;
    p.h = g.h;
  }
  return p;
}

bool execute(const Program& p, const Draw& d, uint8_t* bgra, size_t stride) {
  return detail::run<true>(p, d, bgra, stride);
}

bool execute_scalar(const Program& p, const Draw& d, uint8_t* bgra, size_t stride) {
  return detail::run<false>(p, d, bgra, stride);
}

}  // namespace nsl
}  // namespace neui
