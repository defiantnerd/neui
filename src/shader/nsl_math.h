// Deterministic math for the .nsl executor and compiler (private to
// src/shader/). Scalar functions (m_*) and their 4-wide SIMD mirrors (f4_*)
// perform the same IEEE-754 binary32 operations in the same order, so both
// produce the same bits; the phase-0 benchmark verified this on AppleClang
// arm64, MSVC, GCC and Clang x64. The definitions are the ones
// docs/shader-language.md section 9 refers to.
//
// Include only from TUs built with the neui-shader flags (-ffp-contract=off,
// optimised): a TU that contracts a*b+c into an FMA computes different bits.

#pragma once

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <emmintrin.h>
#define NSL_X86 1
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#include <arm_neon.h>
#define NSL_A64 1
#endif

namespace neui {
namespace nsl {
namespace detail {


// ---------------------------------------------------------------------------
// Deterministic math. Only IEEE-754 binary32 +, -, *, /, sqrt, comparisons and
// float <-> int conversions of in-range values: every one of them is exactly
// specified, so the results are the same bits everywhere. No libm.
// ---------------------------------------------------------------------------

inline uint32_t bits(float f) { uint32_t u; std::memcpy(&u, &f, 4); return u; }
inline float from_bits(uint32_t u) { float f; std::memcpy(&f, &u, 4); return f; }

const float k_inf = std::numeric_limits<float>::infinity();
const float k_nan = std::numeric_limits<float>::quiet_NaN();

inline float m_abs(float x) { return from_bits(bits(x) & 0x7fffffffu); }

// floor: |x| >= 2^23 is already integral; NaN and inf fall through unchanged.
inline float m_floor(float x) {
  const float ax = m_abs(x);
  const float xc = ax < 8388608.0f ? x : 0.0f;  // keep the conversion in range
  const float t  = static_cast<float>(static_cast<int32_t>(xc));
  const float f  = t > xc ? t - 1.0f : t;
  return ax < 8388608.0f ? f : x;
}

// GLSL: min(x, y) = y < x ? y : x; max(x, y) = x < y ? y : x.
inline float m_min(float x, float y) { return y < x ? y : x; }
inline float m_max(float x, float y) { return x < y ? y : x; }

// sin / cos: Cody-Waite reduction by pi/2 (three-part constant), then the
// Cephes single-precision polynomials on [-pi/4, pi/4].
inline void m_sincos_reduce(float x, float& r, int32_t& q) {
  const float k = m_floor(x * 0.636619772f + 0.5f);
  r = ((x - k * 1.5703125f) - k * 4.837512969970703125e-4f) - k * 7.54978995489188216e-8f;
  float m = k - 4.0f * m_floor(k * 0.25f);  // k mod 4, exact; NaN for inf / NaN input
  m = m == m ? m : 0.0f;
  q = static_cast<int32_t>(m);
}
inline float m_sin_poly(float r) {
  const float z = r * r;
  return ((-1.9515295891e-4f * z + 8.3321608736e-3f) * z - 1.6666654611e-1f) * z * r + r;
}
inline float m_cos_poly(float r) {
  const float z = r * r;
  return ((2.443315711809948e-5f * z - 1.388731625493765e-3f) * z + 4.166664568298827e-2f) * z * z
         - 0.5f * z + 1.0f;
}
inline float m_sin(float x) {
  float r; int32_t q; m_sincos_reduce(x, r, q);
  const float s = m_sin_poly(r), c = m_cos_poly(r);
  const float v = (q & 1) ? c : s;
  return (q & 2) ? -v : v;
}
inline float m_cos(float x) {
  float r; int32_t q; m_sincos_reduce(x, r, q);
  const float s = m_sin_poly(r), c = m_cos_poly(r);
  const float v = (q & 1) ? s : c;
  return ((q + 1) & 2) ? -v : v;
}

// exp2: x >= 128 -> +inf; x < -126 -> 0 (never a denormal result); NaN -> NaN.
inline float m_exp2(float x) {
  float xc = x < -126.0f ? -126.0f : x;
  xc = xc > 127.99999f ? 127.99999f : xc;
  xc = xc == xc ? xc : 0.0f;
  const float n = m_floor(xc + 0.5f);  // [-126, 128]
  const float f = xc - n;              // [-0.5, 0.5)
  float p = 1.535336188319500e-4f;
  p = p * f + 1.339887440266574e-3f;
  p = p * f + 9.618437357674640e-3f;
  p = p * f + 5.550332471162809e-2f;
  p = p * f + 2.402264791363012e-1f;
  p = p * f + 6.931472028550421e-1f;
  p = p * f + 1.0f;
  // 2^128 is not a float: scale by 2^127 and double once more.
  const int32_t ni = static_cast<int32_t>(n);
  const float s = from_bits(static_cast<uint32_t>((ni < 128 ? ni : 127) + 127) << 23);
  float r = p * s;
  r = ni == 128 ? r * 2.0f : r;
  r = x < -126.0f ? 0.0f : r;
  r = x >= 128.0f ? k_inf : r;
  return x == x ? r : x;
}

// log2: x < 0 -> NaN; 0 -> -inf; +inf -> +inf; NaN -> NaN; denormals exact.
inline float m_log2(float x) {
  const bool den = x < 1.17549435e-38f;
  const float xs = den ? x * 8388608.0f : x;
  const uint32_t u = bits(xs);
  int32_t e = static_cast<int32_t>((u >> 23) & 0xffu) - 127 - (den ? 23 : 0);
  float m = from_bits((u & 0x007fffffu) | 0x3f800000u);  // [1, 2)
  const bool big = m > 1.41421356f;
  m = big ? m * 0.5f : m;
  e = big ? e + 1 : e;
  const float t  = (m - 1.0f) / (m + 1.0f);
  const float t2 = t * t;
  float p = 0.412198583111132f;
  p = p * t2 + 0.577078016355585f;
  p = p * t2 + 0.961796693925976f;
  p = p * t2 + 2.885390081777927f;
  float r = static_cast<float>(e) + t * p;
  r = x == 0.0f ? -k_inf : r;
  r = x < 0.0f ? k_nan : r;
  r = x == k_inf ? k_inf : r;
  return x == x ? r : x;
}

// atan(y, x): reduce to [0, 1], then to [-tan(pi/8), tan(pi/8)], Cephes
// polynomial; atan(0, 0) = 0.
inline float m_atan2(float y, float x) {
  const float ax = m_abs(x), ay = m_abs(y);
  const float mx = ax > ay ? ax : ay;
  const float mn = ax > ay ? ay : ax;
  const float a  = mx > 0.0f ? mn / mx : 0.0f;
  const bool red = a > 0.414213562f;
  const float aa = red ? (a - 1.0f) / (a + 1.0f) : a;
  const float z  = aa * aa;
  float r = (((8.05374449538e-2f * z - 1.38776856032e-1f) * z + 1.99777106478e-1f) * z
             - 3.33329491539e-1f) * z * aa + aa;
  r = red ? r + 0.785398163f : r;
  r = ay > ax ? 1.57079633f - r : r;
  r = x < 0.0f ? 3.14159265f - r : r;
  r = y < 0.0f ? -r : r;
  return (x == x && y == y) ? r : k_nan;  // a NaN argument gives NaN
}

// neui_hash(vec2): floor, NaN -> 0, clamp to +-2^24, then a 32-bit integer
// hash; the top 24 bits map to [0, 1). Integer-exact, so a GPU emitter (SM4
// has integer ops) produces the same values.
inline uint32_t m_hash_coord(float v) {
  v = v == v ? v : 0.0f;
  v = m_floor(v);
  v = v < -16777216.0f ? -16777216.0f : (v > 16777216.0f ? 16777216.0f : v);
  return static_cast<uint32_t>(static_cast<int32_t>(v));
}
inline float m_hash2(float x, float y) {
  uint32_t h = (m_hash_coord(x) * 0x8da6b343u) ^ (m_hash_coord(y) * 0xd8163841u);
  h ^= h >> 16; h *= 0x7feb352du;
  h ^= h >> 15; h *= 0x846ca68bu;
  h ^= h >> 16;
  return static_cast<float>(h >> 8) * (1.0f / 16777216.0f);
}

// ---------------------------------------------------------------------------
// FP environment. A host (a DAW) may run its UI thread with flush-to-zero /
// denormals-are-zero set; the executor pins round-to-nearest + IEEE
// denormals for the duration of a draw and restores the caller's state.
// ---------------------------------------------------------------------------

struct FpEnvGuard {
#if NSL_X86
  unsigned saved;
  FpEnvGuard() : saved(_mm_getcsr()) { _mm_setcsr(0x1F80u); }
  ~FpEnvGuard() { _mm_setcsr(saved); }
#elif NSL_A64
  uint64_t saved;
  FpEnvGuard() {
    __asm__ volatile("mrs %0, fpcr" : "=r"(saved) : : "memory");
    __asm__ volatile("msr fpcr, %0" : : "r"(uint64_t(0)) : "memory");
  }
  ~FpEnvGuard() { __asm__ volatile("msr fpcr, %0" : : "r"(saved) : "memory"); }
#endif
};

// ---------------------------------------------------------------------------
// Explicit 4-wide SIMD: SSE2 on x86-64, NEON on arm64, plain arrays
// elsewhere. Phase 0 showed that auto-vectorisation of the scalar kernels
// depends on the compiler: clang vectorises everything, GCC part of it, and
// MSVC only add / sub / mul / div / min / max / sqrt / lt. The f4_* math
// mirrors the scalar m_* functions operation for operation, so both produce
// the same bits; the checksums compare them.
// ---------------------------------------------------------------------------

// The SIMD helpers are always inlined into the kernels, whatever a compiler's
// heuristics would decide. (MSVC Debug's /Ob0 would still win, which is why
// src/shader/CMakeLists.txt strips it.)
#if defined(_MSC_VER) && !defined(__clang__)
#define NSL_FI __forceinline
#else
#define NSL_FI inline __attribute__((always_inline))
#endif

// -DNSL_FORCE_PORTABLE builds the portable fallback on any CPU, to test it.
#if NSL_X86 && !defined(NSL_FORCE_PORTABLE)
struct F4 { __m128 v; };
struct I4 { __m128i v; };  // int32 lanes; a mask is all-ones / all-zeros
NSL_FI F4 f4_load(const float* p) { return {_mm_loadu_ps(p)}; }
NSL_FI void f4_store(float* p, F4 a) { _mm_storeu_ps(p, a.v); }
NSL_FI F4 f4(float f) { return {_mm_set1_ps(f)}; }
NSL_FI I4 i4(int32_t i) { return {_mm_set1_epi32(i)}; }
NSL_FI F4 operator+(F4 a, F4 b) { return {_mm_add_ps(a.v, b.v)}; }
NSL_FI F4 operator-(F4 a, F4 b) { return {_mm_sub_ps(a.v, b.v)}; }
NSL_FI F4 operator*(F4 a, F4 b) { return {_mm_mul_ps(a.v, b.v)}; }
NSL_FI F4 operator/(F4 a, F4 b) { return {_mm_div_ps(a.v, b.v)}; }
NSL_FI F4 f4_sqrt(F4 a) { return {_mm_sqrt_ps(a.v)}; }
NSL_FI I4 f4_lt(F4 a, F4 b) { return {_mm_castps_si128(_mm_cmplt_ps(a.v, b.v))}; }
NSL_FI I4 f4_gt(F4 a, F4 b) { return {_mm_castps_si128(_mm_cmpgt_ps(a.v, b.v))}; }
NSL_FI I4 f4_ge(F4 a, F4 b) { return {_mm_castps_si128(_mm_cmpge_ps(a.v, b.v))}; }
NSL_FI I4 f4_le(F4 a, F4 b) { return {_mm_castps_si128(_mm_cmple_ps(a.v, b.v))}; }
NSL_FI void i4_store(void* p, I4 a) { _mm_storeu_si128(static_cast<__m128i*>(p), a.v); }
NSL_FI I4 f4_eq(F4 a, F4 b) { return {_mm_castps_si128(_mm_cmpeq_ps(a.v, b.v))}; }
NSL_FI I4 f4_ne(F4 a, F4 b) { return {_mm_castps_si128(_mm_cmpneq_ps(a.v, b.v))}; }
NSL_FI F4 f4_sel(I4 m, F4 a, F4 b) {
  const __m128 mm = _mm_castsi128_ps(m.v);
  return {_mm_or_ps(_mm_and_ps(mm, a.v), _mm_andnot_ps(mm, b.v))};
}
NSL_FI I4 i4_sel(I4 m, I4 a, I4 b) {
  return {_mm_or_si128(_mm_and_si128(m.v, a.v), _mm_andnot_si128(m.v, b.v))};
}
NSL_FI I4 f4_bits(F4 a) { return {_mm_castps_si128(a.v)}; }
NSL_FI F4 f4_from_bits(I4 a) { return {_mm_castsi128_ps(a.v)}; }
NSL_FI I4 f4_trunc(F4 a) { return {_mm_cvttps_epi32(a.v)}; }  // in-range values only
NSL_FI F4 i4_to_f4(I4 a) { return {_mm_cvtepi32_ps(a.v)}; }
NSL_FI I4 operator+(I4 a, I4 b) { return {_mm_add_epi32(a.v, b.v)}; }
NSL_FI I4 operator-(I4 a, I4 b) { return {_mm_sub_epi32(a.v, b.v)}; }
NSL_FI I4 operator&(I4 a, I4 b) { return {_mm_and_si128(a.v, b.v)}; }
NSL_FI I4 operator|(I4 a, I4 b) { return {_mm_or_si128(a.v, b.v)}; }
NSL_FI I4 operator^(I4 a, I4 b) { return {_mm_xor_si128(a.v, b.v)}; }
template <int N> NSL_FI I4 i4_shl(I4 a) { return {_mm_slli_epi32(a.v, N)}; }
template <int N> NSL_FI I4 i4_shr(I4 a) { return {_mm_srli_epi32(a.v, N)}; }  // logical
NSL_FI I4 i4_lt(I4 a, I4 b) { return {_mm_cmplt_epi32(a.v, b.v)}; }
NSL_FI I4 i4_eq(I4 a, I4 b) { return {_mm_cmpeq_epi32(a.v, b.v)}; }
NSL_FI I4 i4_mul(I4 a, I4 b) {  // low 32 bits; SSE2 has no pmulld
  const __m128i p02 = _mm_mul_epu32(a.v, b.v);
  const __m128i p13 = _mm_mul_epu32(_mm_srli_epi64(a.v, 32), _mm_srli_epi64(b.v, 32));
  return {_mm_unpacklo_epi32(_mm_shuffle_epi32(p02, 0x08), _mm_shuffle_epi32(p13, 0x08))};
}
#elif NSL_A64 && !defined(NSL_FORCE_PORTABLE)
struct F4 { float32x4_t v; };
struct I4 { int32x4_t v; };
NSL_FI F4 f4_load(const float* p) { return {vld1q_f32(p)}; }
NSL_FI void f4_store(float* p, F4 a) { vst1q_f32(p, a.v); }
NSL_FI F4 f4(float f) { return {vdupq_n_f32(f)}; }
NSL_FI I4 i4(int32_t i) { return {vdupq_n_s32(i)}; }
NSL_FI F4 operator+(F4 a, F4 b) { return {vaddq_f32(a.v, b.v)}; }
NSL_FI F4 operator-(F4 a, F4 b) { return {vsubq_f32(a.v, b.v)}; }
NSL_FI F4 operator*(F4 a, F4 b) { return {vmulq_f32(a.v, b.v)}; }
NSL_FI F4 operator/(F4 a, F4 b) { return {vdivq_f32(a.v, b.v)}; }
NSL_FI F4 f4_sqrt(F4 a) { return {vsqrtq_f32(a.v)}; }
NSL_FI I4 m4(uint32x4_t m) { return {vreinterpretq_s32_u32(m)}; }
NSL_FI I4 f4_lt(F4 a, F4 b) { return m4(vcltq_f32(a.v, b.v)); }
NSL_FI I4 f4_gt(F4 a, F4 b) { return m4(vcgtq_f32(a.v, b.v)); }
NSL_FI I4 f4_ge(F4 a, F4 b) { return m4(vcgeq_f32(a.v, b.v)); }
NSL_FI I4 f4_le(F4 a, F4 b) { return m4(vcleq_f32(a.v, b.v)); }
NSL_FI void i4_store(void* p, I4 a) { vst1q_s32(static_cast<int32_t*>(p), a.v); }
NSL_FI I4 f4_eq(F4 a, F4 b) { return m4(vceqq_f32(a.v, b.v)); }
NSL_FI I4 f4_ne(F4 a, F4 b) { return m4(vmvnq_u32(vceqq_f32(a.v, b.v))); }
NSL_FI F4 f4_sel(I4 m, F4 a, F4 b) { return {vbslq_f32(vreinterpretq_u32_s32(m.v), a.v, b.v)}; }
NSL_FI I4 i4_sel(I4 m, I4 a, I4 b) { return {vbslq_s32(vreinterpretq_u32_s32(m.v), a.v, b.v)}; }
NSL_FI I4 f4_bits(F4 a) { return {vreinterpretq_s32_f32(a.v)}; }
NSL_FI F4 f4_from_bits(I4 a) { return {vreinterpretq_f32_s32(a.v)}; }
NSL_FI I4 f4_trunc(F4 a) { return {vcvtq_s32_f32(a.v)}; }  // in-range values only
NSL_FI F4 i4_to_f4(I4 a) { return {vcvtq_f32_s32(a.v)}; }
NSL_FI I4 operator+(I4 a, I4 b) { return {vaddq_s32(a.v, b.v)}; }
NSL_FI I4 operator-(I4 a, I4 b) { return {vsubq_s32(a.v, b.v)}; }
NSL_FI I4 operator&(I4 a, I4 b) { return {vandq_s32(a.v, b.v)}; }
NSL_FI I4 operator|(I4 a, I4 b) { return {vorrq_s32(a.v, b.v)}; }
NSL_FI I4 operator^(I4 a, I4 b) { return {veorq_s32(a.v, b.v)}; }
template <int N> NSL_FI I4 i4_shl(I4 a) { return {vshlq_n_s32(a.v, N)}; }
template <int N> NSL_FI I4 i4_shr(I4 a) {  // logical
  return {vreinterpretq_s32_u32(vshrq_n_u32(vreinterpretq_u32_s32(a.v), N))};
}
NSL_FI I4 i4_lt(I4 a, I4 b) { return m4(vcltq_s32(a.v, b.v)); }
NSL_FI I4 i4_eq(I4 a, I4 b) { return m4(vceqq_s32(a.v, b.v)); }
NSL_FI I4 i4_mul(I4 a, I4 b) { return {vmulq_s32(a.v, b.v)}; }  // low 32 bits
#else
// Portable fallback: four scalars per value.
struct F4 { float v[4]; };
struct I4 { int32_t v[4]; };
#define NSL_F4_LOOP(expr) for (int k_ = 0; k_ < 4; ++k_) { expr; }
NSL_FI F4 f4_load(const float* p) { F4 r; NSL_F4_LOOP(r.v[k_] = p[k_]) return r; }
NSL_FI void f4_store(float* p, F4 a) { NSL_F4_LOOP(p[k_] = a.v[k_]) }
NSL_FI F4 f4(float f) { F4 r; NSL_F4_LOOP(r.v[k_] = f) return r; }
NSL_FI I4 i4(int32_t i) { I4 r; NSL_F4_LOOP(r.v[k_] = i) return r; }
NSL_FI F4 operator+(F4 a, F4 b) { F4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] + b.v[k_]) return r; }
NSL_FI F4 operator-(F4 a, F4 b) { F4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] - b.v[k_]) return r; }
NSL_FI F4 operator*(F4 a, F4 b) { F4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] * b.v[k_]) return r; }
NSL_FI F4 operator/(F4 a, F4 b) { F4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] / b.v[k_]) return r; }
NSL_FI F4 f4_sqrt(F4 a) { F4 r; NSL_F4_LOOP(r.v[k_] = std::sqrt(a.v[k_])) return r; }
NSL_FI I4 f4_lt(F4 a, F4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] < b.v[k_] ? -1 : 0) return r; }
NSL_FI I4 f4_gt(F4 a, F4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] > b.v[k_] ? -1 : 0) return r; }
NSL_FI I4 f4_ge(F4 a, F4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] >= b.v[k_] ? -1 : 0) return r; }
NSL_FI I4 f4_le(F4 a, F4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] <= b.v[k_] ? -1 : 0) return r; }
NSL_FI void i4_store(void* p, I4 a) { std::memcpy(p, a.v, 16); }
NSL_FI I4 f4_eq(F4 a, F4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] == b.v[k_] ? -1 : 0) return r; }
NSL_FI I4 f4_ne(F4 a, F4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] != b.v[k_] ? -1 : 0) return r; }
NSL_FI F4 f4_sel(I4 m, F4 a, F4 b) { F4 r; NSL_F4_LOOP(r.v[k_] = m.v[k_] ? a.v[k_] : b.v[k_]) return r; }
NSL_FI I4 i4_sel(I4 m, I4 a, I4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = m.v[k_] ? a.v[k_] : b.v[k_]) return r; }
NSL_FI I4 f4_bits(F4 a) { I4 r; std::memcpy(r.v, a.v, 16); return r; }
NSL_FI F4 f4_from_bits(I4 a) { F4 r; std::memcpy(r.v, a.v, 16); return r; }
NSL_FI I4 f4_trunc(F4 a) { I4 r; NSL_F4_LOOP(r.v[k_] = static_cast<int32_t>(a.v[k_])) return r; }
NSL_FI F4 i4_to_f4(I4 a) { F4 r; NSL_F4_LOOP(r.v[k_] = static_cast<float>(a.v[k_])) return r; }
NSL_FI uint32_t u_(int32_t x) { return static_cast<uint32_t>(x); }
NSL_FI int32_t s_(uint32_t x) { return static_cast<int32_t>(x); }
NSL_FI I4 operator+(I4 a, I4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = s_(u_(a.v[k_]) + u_(b.v[k_]))) return r; }
NSL_FI I4 operator-(I4 a, I4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = s_(u_(a.v[k_]) - u_(b.v[k_]))) return r; }
NSL_FI I4 operator&(I4 a, I4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] & b.v[k_]) return r; }
NSL_FI I4 operator|(I4 a, I4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] | b.v[k_]) return r; }
NSL_FI I4 operator^(I4 a, I4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] ^ b.v[k_]) return r; }
template <int N> NSL_FI I4 i4_shl(I4 a) { I4 r; NSL_F4_LOOP(r.v[k_] = s_(u_(a.v[k_]) << N)) return r; }
template <int N> NSL_FI I4 i4_shr(I4 a) { I4 r; NSL_F4_LOOP(r.v[k_] = s_(u_(a.v[k_]) >> N)) return r; }
NSL_FI I4 i4_lt(I4 a, I4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] < b.v[k_] ? -1 : 0) return r; }
NSL_FI I4 i4_eq(I4 a, I4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = a.v[k_] == b.v[k_] ? -1 : 0) return r; }
NSL_FI I4 i4_mul(I4 a, I4 b) { I4 r; NSL_F4_LOOP(r.v[k_] = s_(u_(a.v[k_]) * u_(b.v[k_]))) return r; }
#undef NSL_F4_LOOP
#endif

NSL_FI F4 f4_neg(F4 a) { return f4_from_bits(f4_bits(a) ^ i4(INT32_MIN)); }
NSL_FI F4 f4_abs(F4 a) { return f4_from_bits(f4_bits(a) & i4(0x7fffffff)); }
NSL_FI F4 f4_min(F4 x, F4 y) { return f4_sel(f4_lt(y, x), y, x); }
NSL_FI F4 f4_max(F4 x, F4 y) { return f4_sel(f4_lt(x, y), y, x); }

NSL_FI F4 f4_floor(F4 x) {
  const I4 in = f4_lt(f4_abs(x), f4(8388608.0f));
  const F4 xc = f4_sel(in, x, f4(0.0f));
  const F4 t  = i4_to_f4(f4_trunc(xc));
  const F4 f  = f4_sel(f4_gt(t, xc), t - f4(1.0f), t);
  return f4_sel(in, f, x);
}

NSL_FI void f4_sincos_reduce(F4 x, F4& r, I4& q) {
  const F4 k = f4_floor(x * f4(0.636619772f) + f4(0.5f));
  r = ((x - k * f4(1.5703125f)) - k * f4(4.837512969970703125e-4f)) - k * f4(7.54978995489188216e-8f);
  F4 m = k - f4(4.0f) * f4_floor(k * f4(0.25f));
  m = f4_sel(f4_eq(m, m), m, f4(0.0f));
  q = f4_trunc(m);
}
NSL_FI F4 f4_sin_poly(F4 r) {
  const F4 z = r * r;
  return ((f4(-1.9515295891e-4f) * z + f4(8.3321608736e-3f)) * z - f4(1.6666654611e-1f)) * z * r + r;
}
NSL_FI F4 f4_cos_poly(F4 r) {
  const F4 z = r * r;
  return ((f4(2.443315711809948e-5f) * z - f4(1.388731625493765e-3f)) * z + f4(4.166664568298827e-2f)) * z * z
         - f4(0.5f) * z + f4(1.0f);
}
NSL_FI F4 f4_sin(F4 x) {
  F4 r; I4 q; f4_sincos_reduce(x, r, q);
  const F4 s = f4_sin_poly(r), c = f4_cos_poly(r);
  const F4 v = f4_sel(i4_eq(q & i4(1), i4(1)), c, s);
  return f4_sel(i4_eq(q & i4(2), i4(2)), f4_neg(v), v);
}
NSL_FI F4 f4_cos(F4 x) {
  F4 r; I4 q; f4_sincos_reduce(x, r, q);
  const F4 s = f4_sin_poly(r), c = f4_cos_poly(r);
  const F4 v = f4_sel(i4_eq(q & i4(1), i4(1)), s, c);
  return f4_sel(i4_eq((q + i4(1)) & i4(2), i4(2)), f4_neg(v), v);
}

NSL_FI F4 f4_exp2(F4 x) {
  F4 xc = f4_sel(f4_lt(x, f4(-126.0f)), f4(-126.0f), x);
  xc = f4_sel(f4_gt(xc, f4(127.99999f)), f4(127.99999f), xc);
  xc = f4_sel(f4_eq(xc, xc), xc, f4(0.0f));
  const F4 n = f4_floor(xc + f4(0.5f));
  const F4 f = xc - n;
  F4 p = f4(1.535336188319500e-4f);
  p = p * f + f4(1.339887440266574e-3f);
  p = p * f + f4(9.618437357674640e-3f);
  p = p * f + f4(5.550332471162809e-2f);
  p = p * f + f4(2.402264791363012e-1f);
  p = p * f + f4(6.931472028550421e-1f);
  p = p * f + f4(1.0f);
  const I4 ni = f4_trunc(n);
  const I4 e  = i4_sel(i4_lt(ni, i4(128)), ni, i4(127));
  const F4 s  = f4_from_bits(i4_shl<23>(e + i4(127)));
  F4 r = p * s;
  r = f4_sel(i4_eq(ni, i4(128)), r * f4(2.0f), r);
  r = f4_sel(f4_lt(x, f4(-126.0f)), f4(0.0f), r);
  r = f4_sel(f4_ge(x, f4(128.0f)), f4(k_inf), r);
  return f4_sel(f4_eq(x, x), r, x);
}

NSL_FI F4 f4_log2(F4 x) {
  const I4 den = f4_lt(x, f4(1.17549435e-38f));
  const F4 xs = f4_sel(den, x * f4(8388608.0f), x);
  const I4 u = f4_bits(xs);
  I4 e = (i4_shr<23>(u) & i4(0xff)) - i4(127) - (den & i4(23));
  F4 m = f4_from_bits((u & i4(0x007fffff)) | i4(0x3f800000));
  const I4 big = f4_gt(m, f4(1.41421356f));
  m = f4_sel(big, m * f4(0.5f), m);
  e = e + (big & i4(1));
  const F4 t  = (m - f4(1.0f)) / (m + f4(1.0f));
  const F4 t2 = t * t;
  F4 p = f4(0.412198583111132f);
  p = p * t2 + f4(0.577078016355585f);
  p = p * t2 + f4(0.961796693925976f);
  p = p * t2 + f4(2.885390081777927f);
  F4 r = i4_to_f4(e) + t * p;
  r = f4_sel(f4_eq(x, f4(0.0f)), f4(-k_inf), r);
  r = f4_sel(f4_lt(x, f4(0.0f)), f4(k_nan), r);
  r = f4_sel(f4_eq(x, f4(k_inf)), f4(k_inf), r);
  return f4_sel(f4_eq(x, x), r, x);
}

NSL_FI F4 f4_atan2(F4 y, F4 x) {
  const F4 ax = f4_abs(x), ay = f4_abs(y);
  const I4 xbig = f4_gt(ax, ay);
  const F4 mx = f4_sel(xbig, ax, ay);
  const F4 mn = f4_sel(xbig, ay, ax);
  const F4 a  = f4_sel(f4_gt(mx, f4(0.0f)), mn / mx, f4(0.0f));
  const I4 red = f4_gt(a, f4(0.414213562f));
  const F4 aa = f4_sel(red, (a - f4(1.0f)) / (a + f4(1.0f)), a);
  const F4 z  = aa * aa;
  F4 r = (((f4(8.05374449538e-2f) * z - f4(1.38776856032e-1f)) * z + f4(1.99777106478e-1f)) * z
          - f4(3.33329491539e-1f)) * z * aa + aa;
  r = f4_sel(red, r + f4(0.785398163f), r);
  r = f4_sel(f4_gt(ay, ax), f4(1.57079633f) - r, r);
  r = f4_sel(f4_lt(x, f4(0.0f)), f4(3.14159265f) - r, r);
  r = f4_sel(f4_lt(y, f4(0.0f)), f4_neg(r), r);
  return f4_sel(f4_eq(x, x) & f4_eq(y, y), r, f4(k_nan));
}

NSL_FI I4 f4_hash_coord(F4 v) {
  v = f4_sel(f4_eq(v, v), v, f4(0.0f));
  v = f4_floor(v);
  v = f4_sel(f4_lt(v, f4(-16777216.0f)), f4(-16777216.0f),
             f4_sel(f4_gt(v, f4(16777216.0f)), f4(16777216.0f), v));
  return f4_trunc(v);
}
NSL_FI F4 f4_hash2(F4 x, F4 y) {
  I4 h = i4_mul(f4_hash_coord(x), i4(static_cast<int32_t>(0x8da6b343u))) ^
         i4_mul(f4_hash_coord(y), i4(static_cast<int32_t>(0xd8163841u)));
  h = h ^ i4_shr<16>(h); h = i4_mul(h, i4(0x7feb352d));
  h = h ^ i4_shr<15>(h); h = i4_mul(h, i4(static_cast<int32_t>(0x846ca68bu)));
  h = h ^ i4_shr<16>(h);
  return i4_to_f4(i4_shr<8>(h)) * f4(1.0f / 16777216.0f);
}

}  // namespace detail
}  // namespace nsl
}  // namespace neui
