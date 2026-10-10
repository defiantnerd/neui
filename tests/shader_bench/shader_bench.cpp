// Shader phase 0 (plans/shader-effects.md): a prototype of the CPU shader
// executor and of neui's deterministic built-in math. It answers two
// questions before the executor design is frozen:
//
//   1. Cost: ns per pixel for representative shaders, per lane width.
//   2. Determinism: the same bits from every compiler and CPU.
//
// There is no front end yet. Programs are built by hand through a small
// builder that scalarises, folds constant work and allocates registers the way
// the phase-1 compiler will. Phase 1 replaces this file with
// hosts/shared/shader_lang.h + src/shader_exec.cpp and moves the checksums
// into neui_tests.
//
//   neui_shader_bench           report: timings, accuracy, checksums
//   neui_shader_bench --check   checksums only; exit 1 when any differs from
//                               the expected values below (ctest-registered)
//
// Every shader-arithmetic TU must be built without FMA contraction
// (-ffp-contract=off); CMakeLists.txt does that for this one.

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <limits>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <xmmintrin.h>
#define NSB_X86 1
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define NSB_A64 1
#endif

namespace {

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
  return y < 0.0f ? -r : r;
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
#if NSB_X86
  unsigned saved;
  FpEnvGuard() : saved(_mm_getcsr()) { _mm_setcsr(0x1F80u); }
  ~FpEnvGuard() { _mm_setcsr(saved); }
#elif NSB_A64
  uint64_t saved;
  FpEnvGuard() {
    __asm__ volatile("mrs %0, fpcr" : "=r"(saved) : : "memory");
    __asm__ volatile("msr fpcr, %0" : : "r"(uint64_t(0)) : "memory");
  }
  ~FpEnvGuard() { __asm__ volatile("msr fpcr, %0" : : "r"(saved) : "memory"); }
#endif
};

// Simulates a host that flushes denormals on its UI thread.
struct HostFlushToZero {
#if NSB_X86
  unsigned saved;
  HostFlushToZero() : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040u); }  // FTZ | DAZ
  ~HostFlushToZero() { _mm_setcsr(saved); }
#elif NSB_A64
  uint64_t saved;
  HostFlushToZero() {
    __asm__ volatile("mrs %0, fpcr" : "=r"(saved) : : "memory");
    __asm__ volatile("msr fpcr, %0" : : "r"(saved | (uint64_t(1) << 24)) : "memory");  // FZ
  }
  ~HostFlushToZero() { __asm__ volatile("msr fpcr, %0" : : "r"(saved) : "memory"); }
#endif
};

// ---------------------------------------------------------------------------
// IR + builder. The builder hands out SSA values; operations whose operands
// are all constant (literals or uniforms) are folded at build time with the
// same math, which is the "hoist uniform-only work" step. finish() runs a
// linear-scan register allocation in which a destination never shares a
// register with its own operands, so kernels may use __restrict.
// ---------------------------------------------------------------------------

enum Op : uint8_t {
  OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_MIN, OP_MAX, OP_ABS, OP_FLOOR, OP_SQRT,
  OP_SIN, OP_COS, OP_EXP2, OP_LOG2, OP_ATAN2, OP_LT, OP_SEL, OP_HASH2, OP_COUNT
};

inline bool op_is_transcendental(Op op) {
  return op == OP_SIN || op == OP_COS || op == OP_EXP2 || op == OP_LOG2 || op == OP_ATAN2;
}

inline float eval_op(Op op, float a, float b, float c) {
  switch (op) {
    case OP_ADD:   return a + b;
    case OP_SUB:   return a - b;
    case OP_MUL:   return a * b;
    case OP_DIV:   return a / b;
    case OP_MIN:   return m_min(a, b);
    case OP_MAX:   return m_max(a, b);
    case OP_ABS:   return m_abs(a);
    case OP_FLOOR: return m_floor(a);
    case OP_SQRT:  return std::sqrt(a);
    case OP_SIN:   return m_sin(a);
    case OP_COS:   return m_cos(a);
    case OP_EXP2:  return m_exp2(a);
    case OP_LOG2:  return m_log2(a);
    case OP_ATAN2: return m_atan2(a, b);
    case OP_LT:    return a < b ? 1.0f : 0.0f;
    case OP_SEL:   return a != 0.0f ? b : c;
    case OP_HASH2: return m_hash2(a, b);
    default:       return 0.0f;
  }
}

struct Ins { Op op; uint16_t d, a, b, c; };

struct Program {
  const char* name = "";
  std::vector<Ins> code;                          // physical registers
  std::vector<std::pair<uint16_t, float>> consts; // filled once per draw
  int nregs = 0;
  uint16_t px = 0, py = 0;
  uint16_t out[4] = {0, 0, 0, 0};
  int transcendentals = 0;
};

class Builder {
 public:
  struct V { int id; };

  explicit Builder(const char* name) : name_(name) {
    px = input();
    py = input();
  }

  V px, py;

  V k(float v) {
    Val val; val.kind = Val::CONST; val.value = v;
    vals_.push_back(val);
    return V{static_cast<int>(vals_.size()) - 1};
  }

  V op(Op o, V a, V b = V{-1}, V c = V{-1}) {
    const bool fa = is_const(a), fb = b.id < 0 || is_const(b), fc = c.id < 0 || is_const(c);
    if (fa && fb && fc)
      return k(eval_op(o, cval(a), b.id < 0 ? 0.0f : cval(b), c.id < 0 ? 0.0f : cval(c)));
    Val val; val.kind = Val::INS; val.op = o; val.a = a.id; val.b = b.id; val.c = c.id;
    vals_.push_back(val);
    return V{static_cast<int>(vals_.size()) - 1};
  }

  V add(V a, V b) { return op(OP_ADD, a, b); }
  V sub(V a, V b) { return op(OP_SUB, a, b); }
  V mul(V a, V b) { return op(OP_MUL, a, b); }
  V div(V a, V b) { return op(OP_DIV, a, b); }
  V min(V a, V b) { return op(OP_MIN, a, b); }
  V max(V a, V b) { return op(OP_MAX, a, b); }
  V abs(V a) { return op(OP_ABS, a); }
  V floor(V a) { return op(OP_FLOOR, a); }
  V sqrt(V a) { return op(OP_SQRT, a); }
  V sin(V a) { return op(OP_SIN, a); }
  V cos(V a) { return op(OP_COS, a); }
  V exp2(V a) { return op(OP_EXP2, a); }
  V log2(V a) { return op(OP_LOG2, a); }
  V atan2(V y, V x) { return op(OP_ATAN2, y, x); }
  V hash2(V x, V y) { return op(OP_HASH2, x, y); }
  V fract(V a) { return sub(a, floor(a)); }
  V clamp(V a, V lo, V hi) { return min(max(a, lo), hi); }
  V mix(V a, V b, V t) { return add(mul(a, sub(k(1.0f), t)), mul(b, t)); }  // GLSL formula
  V exp(V a) { return exp2(mul(a, k(1.44269504f))); }
  V smoothstep(V e0, V e1, V x) {
    V t = clamp(div(sub(x, e0), sub(e1, e0)), k(0.0f), k(1.0f));
    return mul(mul(t, t), sub(k(3.0f), mul(k(2.0f), t)));
  }

  Program finish(V r, V g, V b, V a) {
    Program pg;
    pg.name = name_;
    const int n = static_cast<int>(vals_.size());
    std::vector<int> last_use(n, -1);
    const V outs[4] = {r, g, b, a};
    for (int i = 0; i < n; ++i) {
      if (vals_[i].kind != Val::INS) continue;
      for (int s : {vals_[i].a, vals_[i].b, vals_[i].c})
        if (s >= 0) last_use[s] = i;
    }
    for (const V& o : outs) last_use[o.id] = n;  // live to the end

    std::vector<int> phys(n, -1);
    std::vector<uint16_t> free_regs;
    int next = 0;
    auto alloc = [&]() -> uint16_t {
      if (!free_regs.empty()) { uint16_t r0 = free_regs.back(); free_regs.pop_back(); return r0; }
      return static_cast<uint16_t>(next++);
    };
    // Inputs and constants are pinned: constants are filled once per draw and
    // the inputs once per block.
    for (int i = 0; i < n; ++i) {
      if (vals_[i].kind == Val::INS) continue;
      if (vals_[i].kind == Val::CONST && last_use[i] < 0) continue;  // folded away
      phys[i] = alloc();
      if (vals_[i].kind == Val::CONST)
        pg.consts.push_back({static_cast<uint16_t>(phys[i]), vals_[i].value});
    }
    pg.px = static_cast<uint16_t>(phys[px.id]);
    pg.py = static_cast<uint16_t>(phys[py.id]);
    for (int i = 0; i < n; ++i) {
      const Val& v = vals_[i];
      if (v.kind != Val::INS) continue;
      if (last_use[i] < 0) continue;  // dead
      Ins in;
      in.op = v.op;
      in.a = static_cast<uint16_t>(phys[v.a]);
      in.b = static_cast<uint16_t>(v.b >= 0 ? phys[v.b] : phys[v.a]);
      in.c = static_cast<uint16_t>(v.c >= 0 ? phys[v.c] : phys[v.a]);
      phys[i] = alloc();
      in.d = static_cast<uint16_t>(phys[i]);
      pg.code.push_back(in);
      if (op_is_transcendental(v.op)) pg.transcendentals++;
      for (int s : {v.a, v.b, v.c}) {
        if (s < 0 || vals_[s].kind != Val::INS || last_use[s] != i) continue;
        free_regs.push_back(static_cast<uint16_t>(phys[s]));
        last_use[s] = -2;  // freed once even when used twice by this instruction
      }
    }
    // A constant or input used directly as an output stays pinned; give every
    // output its own register only if it is not already one.
    for (int c = 0; c < 4; ++c) pg.out[c] = static_cast<uint16_t>(phys[outs[c].id]);
    pg.nregs = next;
    return pg;
  }

 private:
  struct Val {
    enum Kind { INPUT, CONST, INS } kind = INPUT;
    Op op = OP_ADD;
    int a = -1, b = -1, c = -1;
    float value = 0.0f;
  };

  V input() { vals_.push_back(Val{}); return V{static_cast<int>(vals_.size()) - 1}; }
  bool is_const(V v) const { return vals_[v.id].kind == Val::CONST; }
  float cval(V v) const { return vals_[v.id].value; }

  const char* name_;
  std::vector<Val> vals_;
};

// ---------------------------------------------------------------------------
// Executor: a batched structure-of-arrays interpreter. Each instruction runs
// over a lane of L pixels held in float arrays, so dispatch is paid once per
// lane and the inner loops auto-vectorise.
// ---------------------------------------------------------------------------

template <int L>
void run_block(const Program& pg, float (*R)[L]) {
  for (const Ins& in : pg.code) {
    float* __restrict d = R[in.d];
    const float* __restrict a = R[in.a];
    const float* __restrict b = R[in.b];
    const float* __restrict c = R[in.c];
    switch (in.op) {
      case OP_ADD:   for (int i = 0; i < L; ++i) d[i] = a[i] + b[i]; break;
      case OP_SUB:   for (int i = 0; i < L; ++i) d[i] = a[i] - b[i]; break;
      case OP_MUL:   for (int i = 0; i < L; ++i) d[i] = a[i] * b[i]; break;
      case OP_DIV:   for (int i = 0; i < L; ++i) d[i] = a[i] / b[i]; break;
      case OP_MIN:   for (int i = 0; i < L; ++i) d[i] = m_min(a[i], b[i]); break;
      case OP_MAX:   for (int i = 0; i < L; ++i) d[i] = m_max(a[i], b[i]); break;
      case OP_ABS:   for (int i = 0; i < L; ++i) d[i] = m_abs(a[i]); break;
      case OP_FLOOR: for (int i = 0; i < L; ++i) d[i] = m_floor(a[i]); break;
      case OP_SQRT:  for (int i = 0; i < L; ++i) d[i] = std::sqrt(a[i]); break;
      case OP_SIN:   for (int i = 0; i < L; ++i) d[i] = m_sin(a[i]); break;
      case OP_COS:   for (int i = 0; i < L; ++i) d[i] = m_cos(a[i]); break;
      case OP_EXP2:  for (int i = 0; i < L; ++i) d[i] = m_exp2(a[i]); break;
      case OP_LOG2:  for (int i = 0; i < L; ++i) d[i] = m_log2(a[i]); break;
      case OP_ATAN2: for (int i = 0; i < L; ++i) d[i] = m_atan2(a[i], b[i]); break;
      case OP_LT:    for (int i = 0; i < L; ++i) d[i] = a[i] < b[i] ? 1.0f : 0.0f; break;
      case OP_SEL:   for (int i = 0; i < L; ++i) d[i] = a[i] != 0.0f ? b[i] : c[i]; break;
      case OP_HASH2: for (int i = 0; i < L; ++i) d[i] = m_hash2(a[i], b[i]); break;
      default: break;
    }
  }
}

// Output rule: clamp to [0, 1] with NaN -> 0, premultiply in float, then
// round half up to 8 bits. Premultiplied channels never exceed alpha.
inline float clamp01(float v) { return v > 0.0f ? (v < 1.0f ? v : 1.0f) : 0.0f; }
inline uint8_t q8(float v) { return static_cast<uint8_t>(static_cast<int32_t>(v * 255.0f + 0.5f)); }

// Renders a w_px x h_px premultiplied BGRA8 bitmap for a layer of size
// (w_log, h_log) logical px. Pixel (i, j) is evaluated at
// p = ((i + 0.5) * (w_log / w_px), (j + 0.5) * (h_log / h_px)).
template <int L>
void render(const Program& pg, int w_px, int h_px, float w_log, float h_log,
            uint8_t* out, std::vector<float>& regs) {
  FpEnvGuard fp;
  regs.assign(static_cast<size_t>(pg.nregs) * L, 0.0f);
  float (*R)[L] = reinterpret_cast<float (*)[L]>(regs.data());
  for (const auto& kv : pg.consts)
    for (int i = 0; i < L; ++i) R[kv.first][i] = kv.second;
  const float sx = w_log / static_cast<float>(w_px);
  const float sy = h_log / static_cast<float>(h_px);
  for (int y = 0; y < h_px; ++y) {
    const float pyv = (static_cast<float>(y) + 0.5f) * sy;
    for (int x0 = 0; x0 < w_px; x0 += L) {
      for (int i = 0; i < L; ++i) {
        R[pg.px][i] = (static_cast<float>(x0 + i) + 0.5f) * sx;
        R[pg.py][i] = pyv;
      }
      run_block<L>(pg, R);
      const int n = std::min(L, w_px - x0);
      uint8_t* dst = out + (static_cast<size_t>(y) * static_cast<size_t>(w_px) + static_cast<size_t>(x0)) * 4u;
      const float* cr = R[pg.out[0]];
      const float* cg = R[pg.out[1]];
      const float* cb = R[pg.out[2]];
      const float* ca = R[pg.out[3]];
      for (int i = 0; i < n; ++i) {
        const float a = clamp01(ca[i]);
        dst[4 * i + 0] = q8(clamp01(cb[i]) * a);
        dst[4 * i + 1] = q8(clamp01(cg[i]) * a);
        dst[4 * i + 2] = q8(clamp01(cr[i]) * a);
        dst[4 * i + 3] = q8(a);
      }
    }
  }
}

void render_lane(int lane, const Program& pg, int w_px, int h_px, float w_log, float h_log,
                 uint8_t* out, std::vector<float>& regs) {
  switch (lane) {
    case 1:   render<1>(pg, w_px, h_px, w_log, h_log, out, regs); break;
    case 8:   render<8>(pg, w_px, h_px, w_log, h_log, out, regs); break;
    case 32:  render<32>(pg, w_px, h_px, w_log, h_log, out, regs); break;
    case 64:  render<64>(pg, w_px, h_px, w_log, h_log, out, regs); break;
    case 128: render<128>(pg, w_px, h_px, w_log, h_log, out, regs); break;
    default:  render<256>(pg, w_px, h_px, w_log, h_log, out, regs); break;
  }
}

// ---------------------------------------------------------------------------
// Programs, written as the phase-1 front end would scalarise them.
// ---------------------------------------------------------------------------

// ring_glow.nsl from the plan: a value-driven glow ring for a knob.
Program prog_ring_glow(float w, float h, float scale) {
  Builder b("ring_glow");
  using V = Builder::V;
  const float value = 0.65f;
  V cx = b.mul(b.k(w), b.k(0.5f)), cy = b.mul(b.k(h), b.k(0.5f));
  V r  = b.mul(b.min(cx, cy), b.k(0.8f));
  V dx = b.sub(b.px, cx), dy = b.sub(b.py, cy);
  V d  = b.abs(b.sub(b.sqrt(b.add(b.mul(dx, dx), b.mul(dy, dy))), r));
  V npx = b.div(b.k(1.0f), b.k(scale));
  V ring = b.sub(b.k(1.0f), b.smoothstep(b.k(0.0f), b.mul(b.k(1.5f), npx), b.sub(d, b.k(2.0f))));
  V glow = b.exp(b.mul(b.mul(d, b.k(-1.0f)), b.mix(b.k(0.6f), b.k(0.15f), b.k(value))));
  V a = b.clamp(b.add(ring, b.mul(glow, b.k(value))), b.k(0.0f), b.k(1.0f));
  return b.finish(b.k(0.2f), b.k(0.7f), b.k(1.0f), b.mul(a, b.k(1.0f)));
}

// Classic sine plasma: transcendental-heavy.
Program prog_plasma(float w, float h, float /*scale*/) {
  Builder b("plasma");
  using V = Builder::V;
  V t  = b.k(1.7f);
  V ux = b.div(b.px, b.k(w)), uy = b.div(b.py, b.k(h));
  V v1 = b.sin(b.add(b.mul(ux, b.k(10.0f)), t));
  V v2 = b.sin(b.add(b.mul(b.k(10.0f), b.add(b.mul(ux, b.sin(b.div(t, b.k(2.0f)))),
                                                b.mul(uy, b.cos(b.div(t, b.k(3.0f)))))), t));
  V cx = b.add(ux, b.mul(b.k(0.5f), b.sin(b.div(t, b.k(5.0f)))));
  V cy = b.add(uy, b.mul(b.k(0.5f), b.cos(b.div(t, b.k(3.0f)))));
  V v3 = b.sin(b.add(b.sqrt(b.add(b.mul(b.k(100.0f), b.add(b.mul(cx, cx), b.mul(cy, cy))), b.k(1.0f))), t));
  V v  = b.mul(b.add(b.add(v1, v2), v3), b.k(3.14159265f));
  V half = b.k(0.5f);
  V cr = b.add(b.mul(b.sin(v), half), half);
  V cg = b.add(b.mul(b.sin(b.add(v, b.k(2.0943951f))), half), half);
  V cb = b.add(b.mul(b.sin(b.add(v, b.k(4.1887902f))), half), half);
  return b.finish(cr, cg, cb, b.k(1.0f));
}

// Five octaves of value noise (integer hash) mapped onto a two-colour ramp.
Program prog_fbm(float /*w*/, float /*h*/, float /*scale*/) {
  Builder b("fbm5");
  using V = Builder::V;
  V qx = b.div(b.px, b.k(24.0f)), qy = b.div(b.py, b.k(24.0f));
  V sum = b.k(0.0f);
  float amp = 0.5f;
  for (int o = 0; o < 5; ++o) {
    V ix = b.floor(qx), iy = b.floor(qy);
    V fx = b.sub(qx, ix), fy = b.sub(qy, iy);
    V ux = b.mul(b.mul(fx, fx), b.sub(b.k(3.0f), b.mul(b.k(2.0f), fx)));
    V uy = b.mul(b.mul(fy, fy), b.sub(b.k(3.0f), b.mul(b.k(2.0f), fy)));
    V ix1 = b.add(ix, b.k(1.0f)), iy1 = b.add(iy, b.k(1.0f));
    V n = b.mix(b.mix(b.hash2(ix, iy), b.hash2(ix1, iy), ux),
                b.mix(b.hash2(ix, iy1), b.hash2(ix1, iy1), ux), uy);
    sum = b.add(sum, b.mul(b.k(amp), n));
    qx = b.add(b.mul(qx, b.k(2.03f)), b.k(1.7f));
    qy = b.sub(b.mul(qy, b.k(2.03f)), b.k(0.9f));
    amp *= 0.5f;
  }
  V t = b.smoothstep(b.k(0.3f), b.k(0.8f), sum);
  return b.finish(b.mix(b.k(0.08f), b.k(0.95f), t), b.mix(b.k(0.10f), b.k(0.80f), t),
                  b.mix(b.k(0.16f), b.k(0.55f), t), b.k(1.0f));
}

// The plan's budget case: ~150 scalar ops, ~10% transcendental.
Program prog_synthetic(float /*w*/, float /*h*/, float /*scale*/) {
  Builder b("synthetic150");
  using V = Builder::V;
  V x = b.mul(b.px, b.k(0.013f)), y = b.mul(b.py, b.k(0.017f)), acc = b.k(0.0f);
  for (int i = 0; i < 15; ++i) {
    const float k1 = 0.3f + 0.05f * static_cast<float>(i), k2 = 0.7f - 0.03f * static_cast<float>(i);
    V arg = b.add(b.mul(x, b.k(k1)), b.mul(y, b.k(k2)));
    V s;
    switch (i % 4) {
      case 0:  s = b.sin(arg); break;
      case 1:  s = b.atan2(arg, b.add(y, b.k(1.5f))); break;
      case 2:  s = b.exp2(b.mul(b.abs(arg), b.k(-1.0f))); break;
      default: s = b.log2(b.add(b.abs(arg), b.k(1.0f))); break;
    }
    x = b.add(x, b.mul(s, b.k(0.37f)));
    y = b.add(b.min(b.mul(y, b.k(0.93f)), x), s);
    acc = b.add(acc, y);
  }
  return b.finish(b.fract(acc), b.fract(x), b.fract(y), b.k(1.0f));
}

using ProgFn = Program (*)(float, float, float);
const ProgFn k_programs[] = {prog_ring_glow, prog_plasma, prog_fbm, prog_synthetic};
const int k_program_count = 4;

// ---------------------------------------------------------------------------
// Checksums.
// ---------------------------------------------------------------------------

uint32_t crc32(const void* data, size_t len, uint32_t crc = 0) {
  static uint32_t table[256];
  static bool init = false;
  if (!init) {
    for (uint32_t i = 0; i < 256; ++i) {
      uint32_t c = i;
      for (int j = 0; j < 8; ++j) c = (c & 1u) ? 0xEDB88320u ^ (c >> 1) : c >> 1;
      table[i] = c;
    }
    init = true;
  }
  crc = ~crc;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) crc = table[(crc ^ p[i]) & 0xffu] ^ (crc >> 8);
  return ~crc;
}

uint32_t crc_floats(const std::vector<float>& v) {
  uint32_t crc = 0;
  for (float f : v) {
    uint32_t u = f == f ? bits(f) : 0x7fc00000u;  // NaN payload and sign are not observable
    crc = crc32(&u, 4, crc);
  }
  return crc;
}

// Built-in sweeps: raw result bits of every built-in over a fixed set of
// inputs, including denormals. The strongest determinism check.
enum { SW_SIN, SW_COS, SW_EXP2, SW_LOG2, SW_ATAN2, SW_HASH, SW_COUNT };
const char* const k_sweep_names[SW_COUNT] = {"sin", "cos", "exp2", "log2", "atan2", "hash2"};

void sweep_inputs(std::vector<float>& xs, std::vector<float>& ys) {
  xs.clear(); ys.clear();
  uint32_t s = 12345u;
  for (int i = 0; i < 20000; ++i) {
    s = s * 1664525u + 1013904223u;
    const float u = static_cast<float>(s >> 8) * (1.0f / 16777216.0f);  // [0, 1)
    s = s * 1664525u + 1013904223u;
    const float v = static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
    xs.push_back((u - 0.5f) * 400.0f);
    ys.push_back((v - 0.5f) * 400.0f);
  }
  const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, 1e-45f, 1e-40f, 1.1754942e-38f, 1e-30f,
                            1e30f, -1e30f, 3.4e38f, 127.5f, -126.5f, 128.0f, k_inf, -k_inf, k_nan};
  for (float a : specials)
    for (float b2 : specials) { xs.push_back(a); ys.push_back(b2); }
}

void run_sweeps(uint32_t out[SW_COUNT]) {
  FpEnvGuard fp;
  std::vector<float> xs, ys, r;
  sweep_inputs(xs, ys);
  const size_t n = xs.size();
  r.resize(n);
  for (size_t i = 0; i < n; ++i) r[i] = m_sin(xs[i]);
  out[SW_SIN] = crc_floats(r);
  for (size_t i = 0; i < n; ++i) r[i] = m_cos(xs[i]);
  out[SW_COS] = crc_floats(r);
  for (size_t i = 0; i < n; ++i) r[i] = m_exp2(xs[i] * 0.33f);
  out[SW_EXP2] = crc_floats(r);
  for (size_t i = 0; i < n; ++i) r[i] = m_log2(m_abs(xs[i]) * ys[i] * ys[i] * 1e-3f + xs[i]);
  out[SW_LOG2] = crc_floats(r);
  for (size_t i = 0; i < n; ++i) r[i] = m_atan2(ys[i], xs[i]);
  out[SW_ATAN2] = crc_floats(r);
  for (size_t i = 0; i < n; ++i) r[i] = m_hash2(xs[i] * 50.0f, ys[i] * 50.0f);
  out[SW_HASH] = crc_floats(r);
}

// Program output checksums: every program at 64x64 logical px, 1x / 1.5x / 2x.
const float k_check_scales[3] = {1.0f, 1.5f, 2.0f};

void run_programs(int lane, uint32_t out[k_program_count][3]) {
  std::vector<float> regs;
  std::vector<uint8_t> px;
  for (int p = 0; p < k_program_count; ++p) {
    for (int s = 0; s < 3; ++s) {
      const float scale = k_check_scales[s];
      const int wpx = static_cast<int>(64.0f * scale + 0.5f);
      const Program pg = k_programs[p](64.0f, 64.0f, scale);
      px.assign(static_cast<size_t>(wpx) * static_cast<size_t>(wpx) * 4u, 0);
      render_lane(lane, pg, wpx, wpx, 64.0f, 64.0f, px.data(), regs);
      out[p][s] = crc32(px.data(), px.size());
    }
  }
}

// Expected checksums, recorded on macOS / AppleClang arm64. Every other
// compiler and CPU must reproduce them bit for bit.
const uint32_t k_expected_sweeps[SW_COUNT] = {
  0xa7b0f0d9u, 0x29044e7au, 0x874e33a3u, 0xee75588du, 0x1604235bu, 0x63c4a42du};
const uint32_t k_expected_programs[k_program_count][3] = {
  {0x2a9a8213u, 0x54e1557bu, 0xee728ee9u},   // ring_glow
  {0x7c0c8a88u, 0x7b1f3ef3u, 0xc3a5be00u},   // plasma
  {0xb5c47a56u, 0x0aa70a77u, 0xece030ddu},   // fbm5
  {0x2f875d31u, 0x4368de7bu, 0x6d3adc5au}};  // synthetic150

// Returns the number of mismatches; prints every checksum.
int check(bool verbose) {
  int bad = 0;
  uint32_t sw[SW_COUNT];
  run_sweeps(sw);
  for (int i = 0; i < SW_COUNT; ++i) {
    const bool ok = sw[i] == k_expected_sweeps[i];
    bad += ok ? 0 : 1;
    std::printf("  sweep %-6s 0x%08x %s\n", k_sweep_names[i], static_cast<unsigned>(sw[i]), ok ? "ok" : "MISMATCH");
  }
  uint32_t pr[k_program_count][3];
  run_programs(64, pr);
  for (int p = 0; p < k_program_count; ++p) {
    const Program pg = k_programs[p](64.0f, 64.0f, 1.0f);
    for (int s = 0; s < 3; ++s) {
      const bool ok = pr[p][s] == k_expected_programs[p][s];
      bad += ok ? 0 : 1;
      std::printf("  prog  %-12s @%.1fx 0x%08x %s\n", pg.name, static_cast<double>(k_check_scales[s]),
                  static_cast<unsigned>(pr[p][s]), ok ? "ok" : "MISMATCH");
    }
  }

  // The lane width must not change a single bit.
  uint32_t pr1[k_program_count][3];
  run_programs(1, pr1);
  const bool lanes_ok = std::memcmp(pr, pr1, sizeof(pr)) == 0;
  bad += lanes_ok ? 0 : 1;
  std::printf("  lane width 1 vs 64: %s\n", lanes_ok ? "identical" : "DIFFERENT");

  // A host that flushes denormals must not change a single bit either.
  {
    HostFlushToZero host;
    uint32_t sw2[SW_COUNT];
    uint32_t pr2[k_program_count][3];
    run_sweeps(sw2);
    run_programs(64, pr2);
    const bool env_ok = std::memcmp(sw, sw2, sizeof(sw)) == 0 && std::memcmp(pr, pr2, sizeof(pr)) == 0;
    bad += env_ok ? 0 : 1;
    std::printf("  host flush-to-zero, guarded: %s\n", env_ok ? "identical" : "DIFFERENT");
  }
  if (verbose) {
    // Without the guard a flushing host changes results (log2 of a denormal).
    std::vector<float> xs, ys, r1, r2;
    sweep_inputs(xs, ys);
    for (float x : xs) r1.push_back(m_log2(m_abs(x) * 1e-40f));
    {
      HostFlushToZero host;
      for (float x : xs) r2.push_back(m_log2(m_abs(x) * 1e-40f));
    }
    std::printf("  host flush-to-zero, unguarded log2 sweep: %s\n",
                crc_floats(r1) == crc_floats(r2) ? "identical" : "different (the guard is needed)");
  }
  return bad;
}

// ---------------------------------------------------------------------------
// Report.
// ---------------------------------------------------------------------------

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

double ns_per_px(int lane, const Program& pg, int wpx, int hpx, float wl, float hl, int reps) {
  std::vector<float> regs;
  std::vector<uint8_t> px(static_cast<size_t>(wpx) * static_cast<size_t>(hpx) * 4u);
  render_lane(lane, pg, wpx, hpx, wl, hl, px.data(), regs);  // warm up
  double best = 1e30;
  for (int r = 0; r < reps; ++r) {
    const double t0 = now_s();
    render_lane(lane, pg, wpx, hpx, wl, hl, px.data(), regs);
    best = std::min(best, now_s() - t0);
  }
  return best * 1e9 / (static_cast<double>(wpx) * static_cast<double>(hpx));
}

void accuracy() {
  struct Acc { const char* name; double max_err; } acc[5] = {
    {"sin  |x|<=64 (abs)", 0}, {"cos  |x|<=64 (abs)", 0}, {"exp2 [-126,128) (rel)", 0},
    {"log2 (0,1e30] (abs, rel>1)", 0}, {"atan2 (abs)", 0}};
  for (int i = 0; i <= 200000; ++i) {
    const float x = -64.0f + 128.0f * static_cast<float>(i) / 200000.0f;
    acc[0].max_err = std::max(acc[0].max_err, std::fabs(static_cast<double>(m_sin(x)) - std::sin(static_cast<double>(x))));
    acc[1].max_err = std::max(acc[1].max_err, std::fabs(static_cast<double>(m_cos(x)) - std::cos(static_cast<double>(x))));
    const float e = -126.0f + 253.99f * static_cast<float>(i) / 200000.0f;
    const double re = std::exp2(static_cast<double>(e));
    acc[2].max_err = std::max(acc[2].max_err, std::fabs(static_cast<double>(m_exp2(e)) - re) / re);
    const float l = std::pow(10.0f, -44.0f + 74.0f * static_cast<float>(i) / 200000.0f);
    if (l > 0.0f)
      acc[3].max_err = std::max(acc[3].max_err, std::fabs(static_cast<double>(m_log2(l)) - std::log2(static_cast<double>(l))) /
                                                    std::max(1.0, std::fabs(std::log2(static_cast<double>(l)))));
    const float ang = -3.14159f + 6.28318f * static_cast<float>(i) / 200000.0f;
    const float yy = std::sin(ang) * 3.0f, xx = std::cos(ang) * 3.0f;
    acc[4].max_err = std::max(acc[4].max_err, std::fabs(static_cast<double>(m_atan2(yy, xx)) -
                                                        std::atan2(static_cast<double>(yy), static_cast<double>(xx))));
  }
  std::printf("\nBuilt-in accuracy vs double libm:\n");
  for (const Acc& a : acc) std::printf("  %-24s max error %.3g\n", a.name, a.max_err);
}

void report() {
#if defined(_MSC_VER) && !defined(__clang__)
  std::printf("compiler: MSVC %d", _MSC_VER);
#elif defined(__clang__)
  std::printf("compiler: clang %s", __clang_version__);
#elif defined(__GNUC__)
  std::printf("compiler: GCC %s", __VERSION__);
#endif
#if NSB_X86
  std::printf(" (x86, SSE)\n");
#elif NSB_A64
  std::printf(" (arm64)\n");
#else
  std::printf(" (other)\n");
#endif

  const int lanes[] = {1, 8, 32, 64, 128, 256};
  std::printf("\nns per pixel by lane width (120x120 logical knob @2x = 240x240 px, best of 7):\n");
  std::printf("  %-13s %4s %5s", "program", "ops", "trans");
  for (int l : lanes) std::printf(" %7s%-3d", "L=", l);
  std::printf("\n");
  for (int p = 0; p < k_program_count; ++p) {
    const Program pg = k_programs[p](120.0f, 120.0f, 2.0f);
    std::printf("  %-13s %4d %5d", pg.name, static_cast<int>(pg.code.size()), pg.transcendentals);
    for (int l : lanes) std::printf(" %10.2f", ns_per_px(l, pg, 240, 240, 120.0f, 120.0f, 7));
    std::printf("\n");
  }

  std::printf("\nms per re-shade at L=64:\n");
  std::printf("  %-13s %14s %20s %18s\n", "program", "knob 120@2x", "background 800x600@2x", "same @ resolution 0.5");
  for (int p = 0; p < k_program_count; ++p) {
    const Program knob = k_programs[p](120.0f, 120.0f, 2.0f);
    const Program bg   = k_programs[p](800.0f, 600.0f, 2.0f);
    const double knob_ms = ns_per_px(64, knob, 240, 240, 120.0f, 120.0f, 7) * 240.0 * 240.0 * 1e-6;
    const double bg_ms   = ns_per_px(64, bg, 1600, 1200, 800.0f, 600.0f, 2) * 1600.0 * 1200.0 * 1e-6;
    const double half_ms = ns_per_px(64, bg, 800, 600, 800.0f, 600.0f, 3) * 800.0 * 600.0 * 1e-6;
    std::printf("  %-13s %11.2f ms %17.1f ms %15.1f ms\n", knob.name, knob_ms, bg_ms, half_ms);
  }

  accuracy();

  std::printf("\nChecksums:\n");
  const int bad = check(true);
  std::printf("%s\n", bad == 0 ? "all checksums match" : "CHECKSUM MISMATCH (see above)");
}

}  // namespace

int main(int argc, char** argv) {
  if (argc > 1 && std::strcmp(argv[1], "--check") == 0) {
    const int bad = check(false);
    std::printf("%s (%d mismatches)\n", bad == 0 ? "shader bench: checksums match" : "shader bench: CHECKSUM MISMATCH", bad);
    return bad == 0 ? 0 : 1;
  }
  report();
  return 0;
}
