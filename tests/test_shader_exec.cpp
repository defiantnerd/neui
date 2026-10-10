#include "neui_test.h"

// The .nsl CPU executor (src/shader/nsl.h): bit-exact goldens, SIMD vs scalar
// identity, the edge cases of docs/shader-language.md, the pixel grid and
// output rule, and control flow against hand-written C++ equivalents.
#include "shader/nsl.h"

#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

#if defined(__SSE2__) || defined(_M_X64) || (defined(_M_IX86_FP) && _M_IX86_FP >= 2)
#include <xmmintrin.h>
#define SHADER_TEST_X86 1
#elif defined(__aarch64__) && (defined(__GNUC__) || defined(__clang__))
#define SHADER_TEST_A64 1
#endif

using namespace neui_test;
namespace nsl = neui::nsl;

namespace {

const float k_inf = std::numeric_limits<float>::infinity();
const float k_nan = std::numeric_limits<float>::quiet_NaN();

std::shared_ptr<const nsl::Program> build(const std::string& src) {
  nsl::CompileResult r = nsl::compile(src.data(), src.size(), "test.nsl");
  if (!r.program) std::fprintf(stderr, "  compile error: %s\n", r.error.c_str());
  return r.program;
}

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

uint32_t float_bits(float f) {
  uint32_t u;
  std::memcpy(&u, &f, 4);
  return u;
}

// NaN sign and payload are not observable (docs/shader-language.md section 10).
uint32_t crc_floats(const std::vector<float>& v) {
  uint32_t crc = 0;
  for (float f : v) {
    const uint32_t u = f == f ? float_bits(f) : 0x7fc00000u;
    crc = crc32(&u, 4, crc);
  }
  return crc;
}

bool same_bits(const std::vector<float>& a, const std::vector<float>& b) {
  if (a.size() != b.size()) return false;
  for (size_t i = 0; i < a.size(); ++i) {
    const bool na = a[i] != a[i], nb = b[i] != b[i];
    if (na != nb) return false;
    if (!na && float_bits(a[i]) != float_bits(b[i])) return false;
  }
  return true;
}

// Raw return values of main at the given points; checks SIMD == scalar.
std::vector<float> eval(const nsl::Program& p, const std::vector<float>& xs, const std::vector<float>& ys,
                        const nsl::Draw& d = nsl::Draw()) {
  const int n = static_cast<int>(xs.size());
  std::vector<float> simd(static_cast<size_t>(n) * 4u), scalar(simd.size());
  nsl::evaluate(p, d, xs.data(), ys.data(), n, simd.data(), true);
  nsl::evaluate(p, d, xs.data(), ys.data(), n, scalar.data(), false);
  CHECK(same_bits(simd, scalar));
  return simd;
}

// Renders a draw; checks execute == execute_scalar.
std::vector<uint8_t> render(const nsl::Program& p, const nsl::Draw& d) {
  const nsl::Placement pl = nsl::placement(d);
  std::vector<uint8_t> a(static_cast<size_t>(pl.w) * static_cast<size_t>(pl.h) * 4u), b(a.size());
  const size_t stride = static_cast<size_t>(pl.w) * 4u;
  CHECK(nsl::execute(p, d, a.data(), stride));
  CHECK(nsl::execute_scalar(p, d, b.data(), stride));
  CHECK(a == b);
  return a;
}

// Simulates a DAW that flushes denormals on its UI thread.
struct HostFlushToZero {
#if SHADER_TEST_X86
  unsigned saved;
  HostFlushToZero() : saved(_mm_getcsr()) { _mm_setcsr(saved | 0x8040u); }
  ~HostFlushToZero() { _mm_setcsr(saved); }
#elif SHADER_TEST_A64
  uint64_t saved;
  HostFlushToZero() {
    __asm__ volatile("mrs %0, fpcr" : "=r"(saved) : : "memory");
    __asm__ volatile("msr fpcr, %0" : : "r"(saved | (uint64_t(1) << 24)) : "memory");
  }
  ~HostFlushToZero() { __asm__ volatile("msr fpcr, %0" : : "r"(saved) : "memory"); }
#endif
};

// The four phase-0 benchmark programs, in .nsl. Their checksums were first
// recorded by tests/shader_bench and verified identical on AppleClang arm64,
// MSVC x64, GCC 13 x64 and Clang 18 x64.
const char* const k_ring_glow = R"(
uniform float value = 0.65;
uniform vec4  accent = vec4(0.2, 0.7, 1.0, 1.0);
vec4 main(vec2 p) {
  vec2  c    = neui_size * 0.5;
  float r    = min(c.x, c.y) * 0.8;
  float d    = abs(length(p - c) - r);
  float ring = 1.0 - smoothstep(0.0, 1.5 * neui_px, d - 2.0);
  float glow = exp(-d * mix(0.6, 0.15, value));
  float a    = saturate(ring + glow * value);
  return vec4(accent.rgb, a * accent.a);
})";

const char* const k_plasma = R"(
uniform float t = 1.7;
vec4 main(vec2 p) {
  float ux = p.x / neui_size.x, uy = p.y / neui_size.y;
  float v1 = sin(ux * 10.0 + t);
  float v2 = sin(10.0 * (ux * sin(t / 2.0) + uy * cos(t / 3.0)) + t);
  float cx = ux + 0.5 * sin(t / 5.0);
  float cy = uy + 0.5 * cos(t / 3.0);
  float v3 = sin(sqrt(100.0 * (cx * cx + cy * cy) + 1.0) + t);
  float v = (v1 + v2 + v3) * 3.14159265;
  return vec4(sin(v) * 0.5 + 0.5, sin(v + 2.0943951) * 0.5 + 0.5, sin(v + 4.1887902) * 0.5 + 0.5, 1.0);
})";

const char* const k_fbm = R"(
vec4 main(vec2 p) {
  vec2 q = p / 24.0;
  float sum = 0.0;
  float amp = 0.5;
  for (int o = 0; o < 5; o++) {
    sum += amp * neui_noise(q);
    q = vec2(q.x * 2.03 + 1.7, q.y * 2.03 - 0.9);
    amp *= 0.5;
  }
  float t = smoothstep(0.3, 0.8, sum);
  return vec4(mix(0.08, 0.95, t), mix(0.10, 0.80, t), mix(0.16, 0.55, t), 1.0);
})";

const char* const k_synthetic = R"(
vec4 main(vec2 p) {
  float x = p.x * 0.013, y = p.y * 0.017, acc = 0.0;
  for (int i = 0; i < 15; i++) {
    float k1 = 0.3 + 0.05 * float(i);
    float k2 = 0.7 - 0.03 * float(i);
    float arg = x * k1 + y * k2;
    float s;
    float m = mod(float(i), 4.0);
    if (m == 0.0) s = sin(arg);
    else if (m == 1.0) s = atan(arg, y + 1.5);
    else if (m == 2.0) s = exp2(-abs(arg));
    else s = log2(abs(arg) + 1.0);
    x = x + s * 0.37;
    y = min(y * 0.93, x) + s;
    acc = acc + y;
  }
  return vec4(fract(acc), fract(x), fract(y), 1.0);
})";

// The phase-0 sweep inputs: an LCG plus every special value pair.
void sweep_inputs(std::vector<float>& xs, std::vector<float>& ys) {
  xs.clear();
  ys.clear();
  uint32_t s = 12345u;
  for (int i = 0; i < 20000; ++i) {
    s = s * 1664525u + 1013904223u;
    const float u = static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
    s = s * 1664525u + 1013904223u;
    const float v = static_cast<float>(s >> 8) * (1.0f / 16777216.0f);
    xs.push_back((u - 0.5f) * 400.0f);
    ys.push_back((v - 0.5f) * 400.0f);
  }
  const float specials[] = {0.0f, -0.0f, 1.0f, -1.0f, 1e-45f, 1e-40f, 1.1754942e-38f, 1e-30f,
                            1e30f, -1e30f, 3.4e38f, 127.5f, -126.5f, 128.0f, k_inf, -k_inf, k_nan};
  for (float a : specials)
    for (float b : specials) { xs.push_back(a); ys.push_back(b); }
}

std::vector<float> channel(const std::vector<float>& v, int c) {
  std::vector<float> r;
  for (size_t i = static_cast<size_t>(c); i < v.size(); i += 4) r.push_back(v[i]);
  return r;
}

}  // namespace

TEST_CASE("shader exec: benchmark programs reproduce their cross-platform goldens")
{
  const char* const srcs[4] = {k_ring_glow, k_plasma, k_fbm, k_synthetic};
  const uint32_t expect[4][4] = {
    {0x2a9a8213u, 0x54e1557bu, 0xee728ee9u, 0x5df3967cu},
    {0x7c0c8a88u, 0x4b5d1775u, 0xc3a5be00u, 0x31e6247au},
    {0xb5c47a56u, 0x0aa70a77u, 0xece030ddu, 0x2a990a46u},
    {0x2f875d31u, 0x4368de7bu, 0x6d3adc5au, 0x5ef500b2u}};
  for (int p = 0; p < 4; ++p) {
    for (int c = 0; c < 4; ++c) {
      // Case 3: fractional origin (sub-pixel remainder + edge coverage), dithered.
      const std::string src = (c == 3 ? std::string("#pragma neui dither\n") : std::string()) + srcs[p];
      auto prog = build(src);
      REQUIRE(prog != nullptr);
      nsl::Draw d;
      d.w = 64.0f;
      d.h = 64.0f;
      d.scale = c == 0 ? 1.0f : (c == 2 ? 2.0f : 1.5f);
      if (c == 3) { d.x = 10.5f; d.y = 7.25f; }
      const std::vector<uint8_t> px = render(*prog, d);
      CHECK_EQ(crc32(px.data(), px.size()), expect[p][c]);
    }
  }
}

TEST_CASE("shader exec: built-in sweeps reproduce the phase-0 checksums, SIMD and scalar")
{
  std::vector<float> xs, ys;
  sweep_inputs(xs, ys);
  auto a = build("vec4 main(vec2 p) { return vec4(sin(p.x), cos(p.x), exp2(p.x * 0.33), "
                 "log2(abs(p.x) * p.y * p.y * 0.001 + p.x)); }");
  auto b = build("vec4 main(vec2 p) { return vec4(atan(p.y, p.x), neui_hash(p * 50.0), 0.0, 0.0); }");
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  const std::vector<float> ra = eval(*a, xs, ys), rb = eval(*b, xs, ys);
  CHECK_EQ(crc_floats(channel(ra, 0)), 0xa7b0f0d9u);  // sin
  CHECK_EQ(crc_floats(channel(ra, 1)), 0x29044e7au);  // cos
  CHECK_EQ(crc_floats(channel(ra, 2)), 0x874e33a3u);  // exp2
  CHECK_EQ(crc_floats(channel(ra, 3)), 0xee75588du);  // log2
  // atan differs from phase 0 (0x1604235b) on purpose: a NaN argument now
  // gives NaN instead of 0.
  CHECK_EQ(crc_floats(channel(rb, 0)), 0xb436a0fcu);  // atan
  CHECK_EQ(crc_floats(channel(rb, 1)), 0x63c4a42du);  // neui_hash
}

TEST_CASE("shader exec: a host that flushes denormals changes nothing")
{
  std::vector<float> xs, ys;
  sweep_inputs(xs, ys);
  auto a = build("vec4 main(vec2 p) { return vec4(log2(abs(p.x) * 1e-40), exp2(p.y * 0.4), "
                 "p.x * 1e-38, sin(p.y)); }");
  REQUIRE(a != nullptr);
  const std::vector<float> plain = eval(*a, xs, ys);
  auto ring = build(k_ring_glow);
  REQUIRE(ring != nullptr);
  nsl::Draw d;
  d.w = 64.0f;
  d.h = 64.0f;
  d.scale = 1.5f;
  const std::vector<uint8_t> img = render(*ring, d);
  std::vector<float> under_ftz, folded_r;
  bool same_image = false;
  {
    HostFlushToZero host;
    under_ftz = eval(*a, xs, ys);
    same_image = render(*ring, d) == img;
    // Constant folding at compile time is pinned too.
    auto folded = build("vec4 main(vec2 p) { return vec4(log2(1e-40), 1e-38 * 0.01, 0.0, 0.0); }");
    if (folded) folded_r = eval(*folded, {0.0f}, {0.0f});
  }
  // Checked outside the flushing scope: under DAZ a denormal compares as 0.
  CHECK(same_bits(under_ftz, plain));
  CHECK(same_image);
  REQUIRE(folded_r.size() == 4u);
  CHECK(folded_r[0] < -132.0f);  // log2 of a denormal, not of 0
  CHECK(folded_r[1] > 0.0f);     // a denormal result, not flushed
}

TEST_CASE("shader exec: built-ins stay within their accuracy bounds")
{
  std::vector<float> xs, ys;
  for (int i = 0; i <= 20000; ++i) {
    xs.push_back(-64.0f + 128.0f * static_cast<float>(i) / 20000.0f);
    ys.push_back(-125.0f + 252.0f * static_cast<float>(i) / 20000.0f);
  }
  auto p = build("vec4 main(vec2 p) { return vec4(sin(p.x), cos(p.x), exp2(p.y), atan(p.x, 3.0)); }");
  REQUIRE(p != nullptr);
  const std::vector<float> r = eval(*p, xs, ys);
  double e_sin = 0, e_cos = 0, e_exp2 = 0, e_atan = 0;
  for (size_t i = 0; i < xs.size(); ++i) {
    const double x = xs[i], y = ys[i];
    e_sin = std::max(e_sin, std::fabs(r[i * 4 + 0] - std::sin(x)));
    e_cos = std::max(e_cos, std::fabs(r[i * 4 + 1] - std::cos(x)));
    e_exp2 = std::max(e_exp2, std::fabs(r[i * 4 + 2] - std::exp2(y)) / std::exp2(y));
    e_atan = std::max(e_atan, std::fabs(r[i * 4 + 3] - std::atan2(x, 3.0)));
  }
  CHECK(e_sin <= 1e-7);
  CHECK(e_cos <= 1e-7);
  CHECK(e_exp2 <= 1.5e-7);
  CHECK(e_atan <= 3e-7);

  std::vector<float> ls, zero;
  for (int i = 0; i <= 20000; ++i) {
    ls.push_back(std::pow(10.0f, -44.0f + 74.0f * static_cast<float>(i) / 20000.0f));
    zero.push_back(0.0f);
  }
  auto l = build("vec4 main(vec2 p) { return vec4(log2(p.x), 0.0, 0.0, 0.0); }");
  REQUIRE(l != nullptr);
  const std::vector<float> rl = eval(*l, ls, zero);
  double e_log2 = 0;
  for (size_t i = 0; i < ls.size(); ++i) {
    if (!(ls[i] > 0.0f)) continue;
    const double ref = std::log2(static_cast<double>(ls[i]));
    e_log2 = std::max(e_log2, std::fabs(rl[i * 4] - ref) / std::max(1.0, std::fabs(ref)));
  }
  CHECK(e_log2 <= 1.5e-7);
}

namespace {

struct EdgeCase {
  const char* expr;  // a float expression over p.x and p.y
  float x, y;
  float expect;      // NaN means "is NaN"
};

const EdgeCase k_edge_cases[] = {
  {"mod(p.x, p.y)", -1.0f, 3.0f, 2.0f},  // floored, never fmod
  {"mod(p.x, p.y)", 5.5f, -2.0f, -0.5f},
  {"mod(p.x, p.y)", 1.0f, 0.0f, k_nan},
  {"pow(p.x, p.y)", 0.0f, 0.0f, 1.0f},
  {"pow(p.x, p.y)", 0.0f, 2.0f, 0.0f},
  {"pow(p.x, p.y)", 0.0f, -1.0f, k_inf},
  {"pow(p.x, p.y)", -2.0f, 2.0f, k_nan},
  {"smoothstep(p.x, p.x, p.y)", 1.0f, 0.5f, 0.0f},  // degenerate: step
  {"smoothstep(p.x, p.x, p.y)", 1.0f, 1.0f, 1.0f},
  {"normalize(vec2(p.x, p.y)).x", 0.0f, 0.0f, 0.0f},
  {"clamp(0.5, p.x, p.y)", 1.0f, 0.0f, 0.0f},  // lo > hi gives hi
  {"sign(p.x / p.y)", 0.0f, 0.0f, 0.0f},       // sign(NaN) = 0
  {"sign(p.x)", -3.0f, 0.0f, -1.0f},
  {"p.x / p.y", 1.0f, 0.0f, k_inf},
  {"p.x / p.y", -1.0f, 0.0f, -k_inf},
  {"atan(p.x, p.y)", 0.0f, 0.0f, 0.0f},
  {"sqrt(p.x)", -1.0f, 0.0f, k_nan},
  {"log2(p.x)", 0.0f, 0.0f, -k_inf},
  {"log2(p.x)", -1.0f, 0.0f, k_nan},
  {"exp2(p.x)", -126.5f, 0.0f, 0.0f},  // never a denormal
  {"exp2(p.x)", 200.0f, 0.0f, k_inf},
  {"exp2(p.x)", 127.75f, 0.0f, 0.0f},  // checked separately: finite
  {"fract(p.x)", -1e-9f, 0.0f, 1.0f},  // rounds to 1
  {"ceil(p.x)", -1.5f, 0.0f, -1.0f},
  {"min(p.x, p.y)", k_nan, 1.0f, k_nan},
  {"min(p.x, p.y)", 1.0f, k_nan, 1.0f},
  {"max(p.x, p.y)", 1.0f, k_nan, 1.0f},
  {"inversesqrt(p.x)", 4.0f, 0.0f, 0.5f},
  {"step(p.x, p.y)", 1.0f, 1.0f, 1.0f},
  {"step(p.x, p.y)", 1.0f, 0.999f, 0.0f},
  {"float(p.x != p.x)", k_nan, 0.0f, 1.0f},
  {"float(p.x == p.x)", k_nan, 0.0f, 0.0f},
  {"mix(p.x, p.y, 1.0)", 3.0f, 7.0f, 7.0f},
  {"mix(p.x, p.y, 0.0)", 3.0f, 7.0f, 3.0f},
  {"acos(p.x)", 2.0f, 0.0f, k_nan},
  {"length(vec3(p.x, p.y, 0.0))", 3.0f, 4.0f, 5.0f},
  {"dot(vec4(p.x), vec4(p.y))", 2.0f, 3.0f, 24.0f},
  {"reflect(vec2(p.x, p.y), vec2(0.0, 1.0)).y", 1.0f, -1.0f, 1.0f},
  {"cross(vec3(p.x, 0.0, 0.0), vec3(0.0, p.y, 0.0)).z", 2.0f, 3.0f, 6.0f},
  {"neui_hash(vec2(p.x, p.y)) - neui_hash(vec2(0.0))", k_nan, 0.0f, 0.0f},  // NaN hashes like 0
  {"p.x > 0.0 ? 1.0 : 2.0", k_nan, 0.0f, 2.0f},
};

}  // namespace

TEST_CASE("shader exec: the spec's edge cases")
{
  for (const EdgeCase& ec : k_edge_cases) {
    const std::string src = std::string("vec4 main(vec2 p) { return vec4(") + ec.expr + ", 0.0, 0.0, 0.0); }";
    auto p = build(src);
    REQUIRE(p != nullptr);
    const float r = eval(*p, {ec.x}, {ec.y})[0];
    if (std::string(ec.expr) == "exp2(p.x)" && ec.x == 127.75f) {
      CHECK(r > 2.85e38f && r < k_inf);  // 2^127.75, finite
      continue;
    }
    const bool ok = ec.expect != ec.expect ? r != r : float_bits(r) == float_bits(ec.expect) || r == ec.expect;
    if (!ok) std::fprintf(stderr, "  edge case %s at (%g, %g): got %g, expected %g\n", ec.expr,
                          static_cast<double>(ec.x), static_cast<double>(ec.y), static_cast<double>(r),
                          static_cast<double>(ec.expect));
    CHECK(ok);
  }
}

TEST_CASE("shader exec: placement follows the device pixel grid")
{
  nsl::Draw d;
  d.x = 10.5f;
  d.y = 7.25f;
  d.w = 64.0f;
  d.h = 64.0f;
  d.scale = 1.5f;
  nsl::Placement pl = nsl::placement(d);
  CHECK_EQ(pl.x, 15);  // floor(15.75)
  CHECK_EQ(pl.y, 10);  // floor(10.875)
  CHECK_EQ(pl.w, 97);  // ceil(111.75) - 15
  CHECK_EQ(pl.h, 97);  // ceil(106.875) - 10

  d.x = d.y = 0.0f;
  d.scale = 2.0f;
  pl = nsl::placement(d);
  CHECK_EQ(pl.w, 128);
  CHECK_EQ(pl.h, 128);

  nsl::Draw bad = d;
  bad.w = 0.0f;
  CHECK_EQ(nsl::placement(bad).w, 0);
  bad = d;
  bad.scale = 0.0f;
  CHECK_EQ(nsl::placement(bad).w, 0);
  bad = d;
  bad.x = k_nan;
  CHECK_EQ(nsl::placement(bad).w, 0);
  bad = d;
  bad.w = 20000.0f;
  CHECK_EQ(nsl::placement(bad).w, 0);
  auto p = build("vec4 main(vec2 p) { return vec4(1.0); }");
  REQUIRE(p != nullptr);
  uint8_t px[16] = {0};
  CHECK_FALSE(nsl::execute(*p, bad, px, 16));
}

TEST_CASE("shader exec: p is the pixel centre in layer-local logical px")
{
  auto p = build("vec4 main(vec2 p) { return vec4(p, neui_size.x, neui_scale); }");
  REQUIRE(p != nullptr);
  // A layer at a fractional device origin: X0 = 0.5 * 2 = 1.0, Y0 = 0.25 * 2 = 0.5.
  nsl::Draw d;
  d.x = 0.5f;
  d.y = 0.25f;
  d.w = 4.0f;
  d.h = 2.0f;
  d.scale = 2.0f;
  const nsl::Placement pl = nsl::placement(d);
  CHECK_EQ(pl.x, 1);
  CHECK_EQ(pl.y, 0);
  CHECK_EQ(pl.w, 8);
  CHECK_EQ(pl.h, 5);  // rows 0..4 cover [0.5, 4.5)
  // Bitmap pixel (i, j) sits at p = ((i + 0.5 - fx) / s, (j + 0.5 - fy) / s).
  const std::vector<float> r = eval(*p, {(0.0f + 0.5f - 0.0f) / 2.0f}, {(0.0f + 0.5f - 0.5f) / 2.0f}, d);
  CHECK_EQ(r[0], 0.25f);
  CHECK_EQ(r[1], 0.0f);
  CHECK_EQ(r[2], 4.0f);
  CHECK_EQ(r[3], 2.0f);
}

TEST_CASE("shader exec: edge pixels carry their coverage, colour never exceeds alpha")
{
  auto p = build("vec4 main(vec2 p) { return vec4(1.0, 0.5, 0.0, 1.0); }");
  REQUIRE(p != nullptr);
  nsl::Draw d;
  d.x = 0.5f;
  d.w = 2.0f;
  d.h = 1.0f;
  const std::vector<uint8_t> px = render(*p, d);  // columns 0..2 at coverage 0.5, 1, 0.5
  REQUIRE(px.size() == 12u);
  CHECK_EQ(px[3], 128);  // floor(0.5 * 255 + 0.5)
  CHECK_EQ(px[7], 255);
  CHECK_EQ(px[11], 128);
  CHECK_EQ(px[6], 255);  // red
  CHECK_EQ(px[5], 128);  // green 0.5 -> floor(127.5 + 0.5)
  CHECK_EQ(px[4], 0);    // blue
  for (size_t i = 0; i < px.size(); i += 4)
    CHECK(px[i] <= px[i + 3] && px[i + 1] <= px[i + 3] && px[i + 2] <= px[i + 3]);
}

TEST_CASE("shader exec: NaN and infinity at the output")
{
  auto p = build("vec4 main(vec2 p) { float z = p.x - p.x; return vec4(1.0 / z, -1.0 / z, z / z, 1.0); }");
  REQUIRE(p != nullptr);
  nsl::Draw d;
  d.w = 1.0f;
  d.h = 1.0f;
  const std::vector<uint8_t> px = render(*p, d);
  CHECK_EQ(px[2], 255);  // +inf -> 1
  CHECK_EQ(px[1], 0);    // -inf -> 0
  CHECK_EQ(px[0], 0);    // NaN -> 0
  CHECK_EQ(px[3], 255);
}

TEST_CASE("shader exec: rows honour the stride and leave the padding alone")
{
  auto p = build("vec4 main(vec2 p) { return vec4(1.0); }");
  REQUIRE(p != nullptr);
  nsl::Draw d;
  d.w = 3.0f;
  d.h = 2.0f;
  std::vector<uint8_t> buf(2 * 20, 0xAB);
  CHECK(nsl::execute(*p, d, buf.data(), 20));
  CHECK_EQ(buf[0], 255);
  CHECK_EQ(buf[11], 255);
  CHECK_EQ(buf[12], 0xAB);  // padding of row 0
  CHECK_EQ(buf[20], 255);
  CHECK_EQ(buf[39], 0xAB);
}

TEST_CASE("shader exec: uniforms, defaults and the reserved uniforms")
{
  auto p = build("uniform vec4 col = vec4(1.0, 0.0, 0.0, 1.0);\n"
                 "uniform float k;\n"
                 "vec4 main(vec2 p) { return vec4(col.rg, k, neui_px); }");
  REQUIRE(p != nullptr);
  CHECK_EQ(nsl::uniform_components(*p), 5);
  const auto& us = nsl::uniforms(*p);
  REQUIRE(us.size() == 2u);
  CHECK(us[0].name == "col");
  CHECK_EQ(us[0].offset, 0);
  CHECK_EQ(us[0].components, 4);
  CHECK_EQ(us[0].defaults[0], 1.0f);
  CHECK(us[1].name == "k");
  CHECK_EQ(us[1].offset, 4);
  CHECK_EQ(us[1].defaults[0], 0.0f);

  nsl::Draw d;
  d.scale = 4.0f;
  std::vector<float> r = eval(*p, {0.0f}, {0.0f}, d);
  CHECK_EQ(r[0], 1.0f);
  CHECK_EQ(r[1], 0.0f);
  CHECK_EQ(r[2], 0.0f);
  CHECK_EQ(r[3], 0.25f);

  const float values[5] = {0.0f, 1.0f, 0.0f, 1.0f, 0.75f};
  d.uniforms = values;
  r = eval(*p, {0.0f}, {0.0f}, d);
  CHECK_EQ(r[0], 0.0f);
  CHECK_EQ(r[1], 1.0f);
  CHECK_EQ(r[2], 0.75f);
}

TEST_CASE("shader exec: neui_time animates, and only a live read sets the flag")
{
  auto a = build("vec4 main(vec2 p) { return vec4(sin(neui_time), 0.0, 0.0, 1.0); }");
  auto b = build("vec4 main(vec2 p) { float t = neui_time; return vec4(1.0); }");
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  CHECK((nsl::flags(*a) & nsl::NSL_READS_TIME) != 0);
  CHECK((nsl::flags(*b) & nsl::NSL_READS_TIME) == 0);
  nsl::Draw d;
  d.time = 1.0f;
  const float r1 = eval(*a, {0.0f}, {0.0f}, d)[0];
  d.time = 2.0f;
  const float r2 = eval(*a, {0.0f}, {0.0f}, d)[0];
  CHECK(r1 == Approx(0.841470985).epsilon(1e-6));
  CHECK(r2 == Approx(0.909297427).epsilon(1e-6));
}

namespace {

// Control flow, checked against hand-written C++ equivalents (libm math, so
// compared with a tolerance; a lowering bug shows as a gross difference).
struct FlowCase {
  const char* src;
  void (*ref)(float x, float y, float out[4]);
};

float fmodf_floored(float a, float b) { return a - b * std::floor(a / b); }

const FlowCase k_flow_cases[] = {
  {R"(vec4 main(vec2 p) {
        float d = length(p - vec2(16.0));
        if (d > 10.0) return vec4(0.0);
        float a = 1.0 - d / 10.0;
        if (p.x < 12.0) { a = a * 0.5; }
        return vec4(a, d * 0.01, 0.0, 1.0);
      })",
   [](float x, float y, float o[4]) {
     const float d = std::sqrt((x - 16) * (x - 16) + (y - 16) * (y - 16));
     if (d > 10) { o[0] = o[1] = o[2] = o[3] = 0; return; }
     float a = 1 - d / 10;
     if (x < 12) a *= 0.5f;
     o[0] = a; o[1] = d * 0.01f; o[2] = 0; o[3] = 1;
   }},
  {R"(vec4 main(vec2 p) {
        float acc = 0.0; float k = p.x * 0.1;
        for (int i = 0; i < 20; i++) {
          if (float(i) > p.y * 0.5) break;
          if (mod(float(i), 3.0) == 1.0) continue;
          acc += sin(k + float(i));
        }
        return vec4(acc, 0.0, 0.0, 1.0);
      })",
   [](float x, float y, float o[4]) {
     float acc = 0;
     for (int i = 0; i < 20; i++) {
       if (static_cast<float>(i) > y * 0.5f) break;
       if (fmodf_floored(static_cast<float>(i), 3) == 1) continue;
       acc += std::sin(x * 0.1f + static_cast<float>(i));
     }
     o[0] = acc; o[1] = 0; o[2] = 0; o[3] = 1;
   }},
  {R"(void polar(vec2 v, out float r, out float a) {
        r = length(v);
        if (r == 0.0) { a = 0.0; return; }
        a = atan(v.y, v.x);
      }
      float shade(float r, float a) {
        if (r > 20.0) return 0.0;
        return 0.5 + 0.5 * sin(a * 3.0 + r * 0.2);
      }
      vec4 main(vec2 p) {
        float r; float a;
        polar(p - vec2(16.0, 16.0), r, a);
        return vec4(shade(r, a), r, a, 1.0);
      })",
   [](float x, float y, float o[4]) {
     const float vx = x - 16, vy = y - 16;
     const float r = std::sqrt(vx * vx + vy * vy);
     const float a = r == 0 ? 0.0f : std::atan2(vy, vx);
     o[0] = r > 20 ? 0.0f : 0.5f + 0.5f * std::sin(a * 3 + r * 0.2f);
     o[1] = r; o[2] = a; o[3] = 1;
   }},
  {R"(void bump(inout vec3 c, float k) { c.rb = c.br * k; c.g += k; }
      vec4 main(vec2 p) {
        vec3 c = vec3(p.x, p.y, p.x * p.y) * 0.01;
        bump(c, p.x > p.y ? 2.0 : 0.5);
        c.xy = c.yx;
        return vec4(c, p.y > 8.0 && p.x < 20.0 ? 1.0 : 0.25);
      })",
   [](float x, float y, float o[4]) {
     float c[3] = {x * 0.01f, y * 0.01f, x * y * 0.01f};
     const float k = x > y ? 2.0f : 0.5f;
     const float r0 = c[2] * k, b0 = c[0] * k;
     c[0] = r0; c[2] = b0; c[1] += k;
     const float t = c[0]; c[0] = c[1]; c[1] = t;
     o[0] = c[0]; o[1] = c[1]; o[2] = c[2]; o[3] = (y > 8 && x < 20) ? 1.0f : 0.25f;
   }},
  {R"(vec4 main(vec2 p) {
        float s = 0.0;
        for (int i = 0; i < 4; i++) {
          for (int j = 0; j < 4; j++) {
            if (float(j) > p.x * 0.2) break;
            s += float(i * 4 + j);
            if (s > p.y * 3.0) return vec4(s, float(i), float(j), 1.0);
          }
        }
        return vec4(s, -1.0, -1.0, 0.5);
      })",
   [](float x, float y, float o[4]) {
     float s = 0;
     for (int i = 0; i < 4; i++)
       for (int j = 0; j < 4; j++) {
         if (static_cast<float>(j) > x * 0.2f) break;
         s += static_cast<float>(i * 4 + j);
         if (s > y * 3) { o[0] = s; o[1] = static_cast<float>(i); o[2] = static_cast<float>(j); o[3] = 1; return; }
       }
     o[0] = s; o[1] = -1; o[2] = -1; o[3] = 0.5f;
   }},
  {R"(float firsthit(float x, out float steps) {
        steps = 0.0;
        for (int i = 0; i < 16; i++) {
          steps += 1.0;
          if (x + float(i) * 0.7 > 5.0) return float(i);
        }
        return -1.0;
      }
      vec4 main(vec2 p) { float st; float h = firsthit(p.x * 0.3, st); return vec4(h, st, 0.0, 1.0); })",
   [](float x, float, float o[4]) {
     float steps = 0, h = -1;
     for (int i = 0; i < 16; i++) {
       steps += 1;
       if (x * 0.3f + static_cast<float>(i) * 0.7f > 5) { h = static_cast<float>(i); break; }
     }
     o[0] = h; o[1] = steps; o[2] = 0; o[3] = 1;
   }},
};

}  // namespace

TEST_CASE("shader exec: control flow matches hand-written C++")
{
  std::vector<float> xs, ys;
  uint32_t s = 777u;
  for (int i = 0; i < 300; ++i) {
    s = s * 1664525u + 1013904223u;
    xs.push_back(static_cast<float>(s >> 8) * (32.0f / 16777216.0f));
    s = s * 1664525u + 1013904223u;
    ys.push_back(static_cast<float>(s >> 8) * (32.0f / 16777216.0f));
  }
  for (const FlowCase& fc : k_flow_cases) {
    auto p = build(fc.src);
    REQUIRE(p != nullptr);
    const std::vector<float> r = eval(*p, xs, ys);
    int bad = 0;
    for (size_t i = 0; i < xs.size(); ++i) {
      float want[4];
      fc.ref(xs[i], ys[i], want);
      for (int c = 0; c < 4; ++c)
        if (std::fabs(r[i * 4 + static_cast<size_t>(c)] - want[c]) > 1e-4f * (1.0f + std::fabs(want[c]))) ++bad;
    }
    if (bad) std::fprintf(stderr, "  flow case mismatched %d values:\n%s\n", bad, fc.src);
    CHECK_EQ(bad, 0);
  }
}

TEST_CASE("shader exec: uniform-only work runs once per draw")
{
  auto p = build("uniform float t = 1.0;\n"
                 "vec4 main(vec2 p) { float k = sin(t) * cos(t * 2.0) + exp(t); return vec4(p.x * k); }");
  REQUIRE(p != nullptr);
  const nsl::Stats st = nsl::stats(*p);
  CHECK(st.draw_ops >= 5);
  CHECK_EQ(st.pixel_ops, 1);  // only p.x * k is per pixel
  CHECK_EQ(st.transcendentals, 0);
}
