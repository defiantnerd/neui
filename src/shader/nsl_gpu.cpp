// .nsl GPU form: the pixel program as an HLSL ps_4_0 pixel shader for a
// zero-input Direct2D custom effect (plans/shader-effects.md, "GPU
// executors"). The draw program stays on the CPU: gpu_constants()
// (nsl_exec.cpp) runs it and passes its values in the constant buffer, so
// everything evaluated once per draw is bit-identical to the CPU path.
//
// The shader reproduces the CPU executor operation for operation: neui's own
// polynomials and hash (not the GPU's intrinsics), the spec's pixel grid,
// coverage, rounding and dithering. Every variable is `precise`, so the
// compiler does not fuse a*b+c or reorder; what remains different is the
// GPU's division, sqrt and denormal handling, which the fidelity contract
// allows for (docs/shader-language.md section 12).

#include "nsl_ir.h"

#include <cstdio>
#include <string>
#include <vector>

namespace neui {
namespace nsl {
namespace {

using namespace detail;

// A float as an exact HLSL expression: the literal's bits.
std::string H(float f) {
  char buf[32];
  std::snprintf(buf, sizeof buf, "asfloat(0x%08xu)", static_cast<unsigned>(bits(f)));
  return buf;
}

std::string reg(uint16_t r) { return "r" + std::to_string(r); }

// The constant buffer as HLSL: float index i is c[i / 4].xyzw[i % 4].
std::string cb(int i) {
  static const char k_comp[] = "xyzw";
  return "c[" + std::to_string(i / 4) + "]." + k_comp[i % 4];
}

// neui's math (nsl_math.h), in HLSL. The constants are the same floats.
std::string helpers(uint32_t used) {
  std::string s;
  auto uses = [&](Op op) { return (used & (1u << op)) != 0; };
  const std::string INF = H(k_inf), NAN_ = H(k_nan);

  if (uses(OP_SIN) || uses(OP_COS)) {
    s += "void n_reduce(float x, out float r, out int q) {\n"
         "  precise float k = floor(x * " + H(0.636619772f) + " + 0.5);\n"
         "  precise float rr = ((x - k * " + H(1.5703125f) + ") - k * " + H(4.837512969970703125e-4f) +
         ") - k * " + H(7.54978995489188216e-8f) + ";\n"
         "  precise float m = k - 4.0 * floor(k * 0.25);\n"
         "  m = isnan(m) ? 0.0 : m;\n"
         "  r = rr;\n"
         "  q = (int)m;\n"
         "}\n"
         "float n_sin_poly(float r) {\n"
         "  precise float z = r * r;\n"
         "  precise float v = ((" + H(-1.9515295891e-4f) + " * z + " + H(8.3321608736e-3f) + ") * z - " +
         H(1.6666654611e-1f) + ") * z * r + r;\n"
         "  return v;\n"
         "}\n"
         "float n_cos_poly(float r) {\n"
         "  precise float z = r * r;\n"
         "  precise float v = ((" + H(2.443315711809948e-5f) + " * z - " + H(1.388731625493765e-3f) + ") * z + " +
         H(4.166664568298827e-2f) + ") * z * z - 0.5 * z + 1.0;\n"
         "  return v;\n"
         "}\n";
  }
  if (uses(OP_SIN))
    s += "float n_sin(float x) {\n"
         "  float r; int q; n_reduce(x, r, q);\n"
         "  precise float v = (q & 1) != 0 ? n_cos_poly(r) : n_sin_poly(r);\n"
         "  return (q & 2) != 0 ? -v : v;\n"
         "}\n";
  if (uses(OP_COS))
    s += "float n_cos(float x) {\n"
         "  float r; int q; n_reduce(x, r, q);\n"
         "  precise float v = (q & 1) != 0 ? n_sin_poly(r) : n_cos_poly(r);\n"
         "  return ((q + 1) & 2) != 0 ? -v : v;\n"
         "}\n";
  if (uses(OP_EXP2))
    s += "float n_exp2(float x) {\n"
         "  precise float xc = x < -126.0 ? -126.0 : x;\n"
         "  xc = xc > " + H(127.99999f) + " ? " + H(127.99999f) + " : xc;\n"
         "  xc = isnan(xc) ? 0.0 : xc;\n"
         "  precise float n = floor(xc + 0.5);\n"
         "  precise float f = xc - n;\n"
         "  precise float p = " + H(1.535336188319500e-4f) + ";\n"
         "  p = p * f + " + H(1.339887440266574e-3f) + ";\n"
         "  p = p * f + " + H(9.618437357674640e-3f) + ";\n"
         "  p = p * f + " + H(5.550332471162809e-2f) + ";\n"
         "  p = p * f + " + H(2.402264791363012e-1f) + ";\n"
         "  p = p * f + " + H(6.931472028550421e-1f) + ";\n"
         "  p = p * f + 1.0;\n"
         "  int ni = (int)n;\n"
         "  precise float sc = asfloat((uint)((ni < 128 ? ni : 127) + 127) << 23);\n"
         "  precise float r = p * sc;\n"
         "  r = ni == 128 ? r * 2.0 : r;\n"
         "  r = x < -126.0 ? 0.0 : r;\n"
         "  r = x >= 128.0 ? " + INF + " : r;\n"
         "  return isnan(x) ? x : r;\n"
         "}\n";
  if (uses(OP_LOG2))
    s += "float n_log2(float x) {\n"
         "  bool den = x < " + H(1.17549435e-38f) + ";\n"
         "  precise float xs = den ? x * 8388608.0 : x;\n"
         "  uint u = asuint(xs);\n"
         "  int e = (int)((u >> 23) & 0xffu) - 127 - (den ? 23 : 0);\n"
         "  precise float m = asfloat((u & 0x007fffffu) | 0x3f800000u);\n"
         "  bool big = m > " + H(1.41421356f) + ";\n"
         "  m = big ? m * 0.5 : m;\n"
         "  e = big ? e + 1 : e;\n"
         "  precise float t = (m - 1.0) / (m + 1.0);\n"
         "  precise float t2 = t * t;\n"
         "  precise float p = " + H(0.412198583111132f) + ";\n"
         "  p = p * t2 + " + H(0.577078016355585f) + ";\n"
         "  p = p * t2 + " + H(0.961796693925976f) + ";\n"
         "  p = p * t2 + " + H(2.885390081777927f) + ";\n"
         "  precise float r = (float)e + t * p;\n"
         "  r = x == 0.0 ? -" + INF + " : r;\n"
         "  r = x < 0.0 ? " + NAN_ + " : r;\n"
         "  r = x == " + INF + " ? " + INF + " : r;\n"
         "  return isnan(x) ? x : r;\n"
         "}\n";
  if (uses(OP_ATAN2))
    s += "float n_atan2(float y, float x) {\n"
         "  precise float ax = abs(x), ay = abs(y);\n"
         "  precise float mx = ax > ay ? ax : ay;\n"
         "  precise float mn = ax > ay ? ay : ax;\n"
         "  precise float a = mx > 0.0 ? mn / mx : 0.0;\n"
         "  bool red = a > " + H(0.414213562f) + ";\n"
         "  precise float aa = red ? (a - 1.0) / (a + 1.0) : a;\n"
         "  precise float z = aa * aa;\n"
         "  precise float r = (((" + H(8.05374449538e-2f) + " * z - " + H(1.38776856032e-1f) + ") * z + " +
         H(1.99777106478e-1f) + ") * z - " + H(3.33329491539e-1f) + ") * z * aa + aa;\n"
         "  r = red ? r + " + H(0.785398163f) + " : r;\n"
         "  r = ay > ax ? " + H(1.57079633f) + " - r : r;\n"
         "  r = x < 0.0 ? " + H(3.14159265f) + " - r : r;\n"
         "  r = y < 0.0 ? -r : r;\n"
         "  return (isnan(x) || isnan(y)) ? " + NAN_ + " : r;\n"
         "}\n";
  if (uses(OP_HASH2))
    s += "uint n_hash_coord(float v) {\n"
         "  v = isnan(v) ? 0.0 : floor(v);\n"
         "  v = v < -16777216.0 ? -16777216.0 : (v > 16777216.0 ? 16777216.0 : v);\n"
         "  return asuint((int)v);\n"
         "}\n"
         "float n_hash2(float x, float y) {\n"
         "  uint h = (n_hash_coord(x) * 0x8da6b343u) ^ (n_hash_coord(y) * 0xd8163841u);\n"
         "  h ^= h >> 16; h *= 0x7feb352du;\n"
         "  h ^= h >> 15; h *= 0x846ca68bu;\n"
         "  h ^= h >> 16;\n"
         "  return (float)(h >> 8) * " + H(1.0f / 16777216.0f) + ";\n"
         "}\n";
  return s;
}

// The output rule (spec section 11), as the CPU's pack4 computes it, with the
// 8-bit values handed to the UNORM target as exact multiples of 1/255.
std::string pack(bool dither) {
  std::string s;
  if (dither)
    s += "static const float n_bayer[16] = { 0, 8, 2, 10, 12, 4, 14, 6, 3, 11, 1, 9, 15, 7, 13, 5 };\n";
  s += "float n_clamp01(float v) { return v > 0.0 ? (v < 1.0 ? v : 1.0) : 0.0; }\n"
       "float4 n_pack(float r, float g, float b, float a, float ix, float iy) {\n"
       "  precise float cx = (c[0].z < ix + 1.0 ? c[0].z : ix + 1.0) - (ix < c[0].x ? c[0].x : ix);\n"
       "  precise float cy = (c[0].w < iy + 1.0 ? c[0].w : iy + 1.0) - (iy < c[0].y ? c[0].y : iy);\n"
       "  precise float al = n_clamp01(a) * (cx * cy);\n"
       "  precise float a8 = floor(al * 255.0 + 0.5);\n";
  s += dither ? "  precise float d = (n_bayer[((uint)iy & 3u) * 4u + ((uint)ix & 3u)] + 0.5) / 16.0;\n"
              : "  precise float d = 0.5;\n";
  s += "  precise float qr = floor(n_clamp01(r) * al * 255.0 + d);\n"
       "  precise float qg = floor(n_clamp01(g) * al * 255.0 + d);\n"
       "  precise float qb = floor(n_clamp01(b) * al * 255.0 + d);\n"
       "  qr = a8 < qr ? a8 : qr;\n"
       "  qg = a8 < qg ? a8 : qg;\n"
       "  qb = a8 < qb ? a8 : qb;\n"
       "  return float4(qr, qg, qb, a8) * (c[1].z / 255.0);\n"
       "}\n";
  return s;
}

}  // namespace

std::string emit_hlsl(const Program& p) {
  const size_t floats = gpu_constant_count(p);
  if (!floats) return std::string();
  const std::vector<int> slot = gpu_pool_slots(p);

  uint32_t used = 0;
  for (const Ins& in : p.pixel_code) used |= 1u << in.op;

  std::string s;
  s.reserve(4096 + p.pixel_code.size() * 32);
  s += "// Generated by neui from a .nsl program (src/shader/nsl_gpu.cpp).\n";
  s += "cbuffer C : register(b0) { float4 c[" + std::to_string(floats / 4) + "]; };\n";
  s += helpers(used);
  s += pack((p.flag_bits & NSL_DITHER) != 0);
  s += "float4 main(float4 pos : SV_POSITION, float4 scene : SCENE_POSITION) : SV_Target {\n";
  s += "  precise float ix = floor(scene.x), iy = floor(scene.y);\n";
  if (p.pixel_regs > 0) {
    s += "  precise float ";
    for (int r = 0; r < p.pixel_regs; ++r) {
      if (r) s += r % 16 ? ", " : ",\n    ";
      s += reg(static_cast<uint16_t>(r));
    }
    s += ";\n";
  }
  // The pixel grid, as the CPU computes p for column i and row j.
  s += "  " + reg(p.px) + " = ((ix + 0.5) - c[0].x) / c[1].x;\n";
  s += "  " + reg(p.py) + " = ((iy + 0.5) - c[0].y) / c[1].x;\n";
  for (const Ins& in : p.pixel_code) {
    const std::string d = reg(in.d), a = reg(in.a), b = reg(in.b), c = reg(in.c);
    std::string e;
    switch (in.op) {
      case OP_ADD:   e = a + " + " + b; break;
      case OP_SUB:   e = a + " - " + b; break;
      case OP_MUL:   e = a + " * " + b; break;
      case OP_DIV:   e = a + " / " + b; break;
      case OP_NEG:   e = "-" + a; break;
      case OP_MIN:   e = "(" + b + " < " + a + " ? " + b + " : " + a + ")"; break;
      case OP_MAX:   e = "(" + a + " < " + b + " ? " + b + " : " + a + ")"; break;
      case OP_ABS:   e = "abs(" + a + ")"; break;
      case OP_FLOOR: e = "floor(" + a + ")"; break;
      case OP_SQRT:  e = "sqrt(" + a + ")"; break;
      case OP_SIN:   e = "n_sin(" + a + ")"; break;
      case OP_COS:   e = "n_cos(" + a + ")"; break;
      case OP_EXP2:  e = "n_exp2(" + a + ")"; break;
      case OP_LOG2:  e = "n_log2(" + a + ")"; break;
      case OP_ATAN2: e = "n_atan2(" + a + ", " + b + ")"; break;
      case OP_HASH2: e = "n_hash2(" + a + ", " + b + ")"; break;
      case OP_LT:    e = "(" + a + " < " + b + " ? 1.0 : 0.0)"; break;
      case OP_LE:    e = "(" + a + " <= " + b + " ? 1.0 : 0.0)"; break;
      case OP_EQ:    e = "(" + a + " == " + b + " ? 1.0 : 0.0)"; break;
      case OP_NE:    e = "(" + a + " != " + b + " ? 1.0 : 0.0)"; break;
      case OP_SEL:   e = "(" + a + " != 0.0 ? " + b + " : " + c + ")"; break;
      case OP_LOAD: {
        const int sl = slot[in.a];
        e = sl >= 0 ? cb(sl) : H(p.pixel_pool[in.a].value);
        break;
      }
      default: return std::string();
    }
    s += "  " + d + " = " + e + ";\n";
  }
  s += "  return n_pack(" + reg(p.out[0]) + ", " + reg(p.out[1]) + ", " + reg(p.out[2]) + ", " +
       reg(p.out[3]) + ", ix, iy);\n";
  s += "}\n";
  return s;
}

}  // namespace nsl
}  // namespace neui
