// Shader executor benchmark: compiles a few .nsl programs with the real
// compiler (src/shader/nsl.h) and times execute() at widget and background
// sizes. Report only; correctness and the cross-platform goldens live in
// neui_tests (test_shader_exec.cpp).
//
//   neui_shader_bench

#include "shader/nsl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

namespace nsl = neui::nsl;

// The bitmap bound to a shader's sampler(s), if any.
enum Tex { TEX_NONE, TEX_PHOTO, TEX_NORMAL, TEX_NOISE };

struct Shader {
  const char* name;
  const char* src;
  Tex tex = TEX_NONE;
};

const Shader k_shaders[] = {
  // No per-pixel work: measures the fixed cost per pixel (block loop, output
  // packing, coverage).
  {"flat", R"(
uniform vec4 col = vec4(0.2, 0.4, 0.6, 1.0);
vec4 main(vec2 p) { return col; }
)"},
  {"ring_glow", R"(
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
})"},
  {"plasma", R"(
vec4 main(vec2 p) {
  vec2 u = p / neui_size;
  float t = neui_time;
  float v = sin(u.x * 10.0 + t) + sin(10.0 * (u.x * sin(t / 2.0) + u.y * cos(t / 3.0)) + t);
  vec2 c = u + 0.5 * vec2(sin(t / 5.0), cos(t / 3.0));
  v += sin(sqrt(100.0 * dot(c, c) + 1.0) + t);
  v *= 3.14159265;
  return vec4(0.5 + 0.5 * sin(v), 0.5 + 0.5 * sin(v + 2.0943951), 0.5 + 0.5 * sin(v + 4.1887902), 1.0);
})"},
  {"fbm5", R"(
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
})"},
  {"metal_knob", R"(
uniform float value = 0.4;
const float PI = 3.14159265;
float ring(float d, float r, float w) { return 1.0 - smoothstep(w - neui_px, w, abs(d - r)); }
vec4 main(vec2 p) {
  vec2 c = neui_size * 0.5;
  vec2 v = p - c;
  float d = length(v);
  float R = min(c.x, c.y) * 0.9;
  if (d > R + 1.0) return vec4(0.0);
  float ang = atan(v.x, -v.y);
  float lit = 0.55 + 0.25 * cos(ang * 2.0) + 0.04 * neui_noise(vec2(ang * 40.0, d * 0.5));
  vec3 col = vec3(lit) * (0.85 + 0.15 * (1.0 - d / R));
  float sweep = (ang + PI * 0.75) / (PI * 1.5);
  float arc = ring(d, R * 0.82, 2.5) * step(sweep, value) * step(0.0, sweep);
  col = mix(col, vec3(0.2, 0.75, 1.0), arc);
  float tick = ring(d, R * 0.82, 1.0) * (1.0 - step(0.08, abs(mod(sweep * 10.0 + 0.5, 1.0) - 0.5)));
  col = mix(col, vec3(0.1), tick * 0.6);
  float edge = 1.0 - smoothstep(R - neui_px, R, d);
  return vec4(col, edge);
})"},
  // Texture sampling (plans/shader-effects.md, phase 9).
  {"tex_copy", R"(
uniform sampler2D img;
vec4 main(vec2 p) { return texture2D(img, p / neui_size); })", TEX_PHOTO},
  {"tex_nearest", R"(
#pragma neui sampler img nearest
uniform sampler2D img;
vec4 main(vec2 p) { return texture2D(img, p / neui_size); })", TEX_PHOTO},
  {"tex_blur9", R"(
uniform sampler2D img;
vec4 main(vec2 p) {
  vec2 uv = p / neui_size;
  vec2 t = neui_texel(img);
  vec4 sum = vec4(0.0);
  for (int y = -1; y <= 1; y++)
    for (int x = -1; x <= 1; x++)
      sum += texture2D(img, uv + vec2(float(x), float(y)) * t);
  return sum / 9.0;
})", TEX_PHOTO},
  {"tex_normal", R"(
uniform float value = 0.4;
uniform sampler2D normals;
vec4 main(vec2 p) {
  vec2 c = neui_size * 0.5;
  float d = length(p - c);
  float R = min(c.x, c.y) * 0.9;
  vec3 n = texture2D(normals, p / neui_size).xyz * 2.0 - 1.0;
  float a = (value - 0.5) * 4.71238898;
  vec3 l = normalize(vec3(sin(a), -cos(a), 1.2));
  float lit = saturate(dot(n, l));
  float spec = pow(saturate(dot(reflect(-l, n), vec3(0.0, 0.0, 1.0))), 24.0);
  vec3 col = vec3(0.18, 0.2, 0.24) + vec3(0.6) * lit + vec3(0.5) * spec;
  return vec4(col, 1.0 - smoothstep(R - neui_px, R, d));
})", TEX_NORMAL},
  {"tex_noise", R"(
#pragma neui sampler noise repeat
uniform sampler2D noise;
vec4 main(vec2 p) {
  float sum = texture2D(noise, p / 256.0).r;
  float t = smoothstep(0.3, 0.8, sum);
  return vec4(mix(0.08, 0.95, t), mix(0.10, 0.80, t), mix(0.16, 0.55, t), 1.0);
})", TEX_NOISE},
};

// Premultiplied BGRA test bitmaps.
struct Bitmap {
  int w = 0, h = 0;
  std::vector<uint8_t> px;
};

Bitmap photo() {  // random, half transparent: the worst case for unpremultiply
  Bitmap b;
  b.w = b.h = 512;
  b.px.resize(512u * 512u * 4u);
  uint32_t s = 1u;
  for (size_t i = 0; i < b.px.size(); i += 4) {
    s = s * 1664525u + 1013904223u;
    const uint32_t a = s >> 24;
    for (int c = 0; c < 3; ++c) b.px[i + static_cast<size_t>(c)] = static_cast<uint8_t>(((s >> (8 * c)) & 0xffu) * a / 255u);
    b.px[i + 3] = static_cast<uint8_t>(a);
  }
  return b;
}

Bitmap normal_map() {  // a domed knob with machined rings
  Bitmap b;
  b.w = b.h = 256;
  b.px.resize(256u * 256u * 4u);
  for (int y = 0; y < 256; ++y)
    for (int x = 0; x < 256; ++x) {
      const double u = (x + 0.5) / 128.0 - 1.0, v = (y + 0.5) / 128.0 - 1.0;
      const double r = std::sqrt(u * u + v * v);
      const double ring = 0.05 * std::sin(r * 60.0);
      double nx = u * 0.6 + ring * u, ny = v * 0.6 + ring * v, nz = 1.0;
      const double l = std::sqrt(nx * nx + ny * ny + nz * nz);
      nx /= l; ny /= l; nz /= l;
      uint8_t* q = &b.px[(static_cast<size_t>(y) * 256u + static_cast<size_t>(x)) * 4u];
      q[2] = static_cast<uint8_t>(127.5 + 127.5 * nx);
      q[1] = static_cast<uint8_t>(127.5 + 127.5 * ny);
      q[0] = static_cast<uint8_t>(127.5 + 127.5 * nz);
      q[3] = 255;
    }
  return b;
}

Bitmap noise_tile() {  // fbm5 baked once into a 256 x 256 tile
  Bitmap b;
  b.w = b.h = 256;
  b.px.resize(256u * 256u * 4u);
  const char* src = R"(
vec4 main(vec2 p) {
  vec2 q = p / 24.0;
  float sum = 0.0;
  float amp = 0.5;
  for (int o = 0; o < 5; o++) {
    sum += amp * neui_noise(q);
    q = vec2(q.x * 2.03 + 1.7, q.y * 2.03 - 0.9);
    amp *= 0.5;
  }
  return vec4(vec3(sum), 1.0);
})";
  const nsl::CompileResult r = nsl::compile(src, std::strlen(src), "noise_tile");
  nsl::Draw d;
  d.w = d.h = 256.0f;
  if (r.program) nsl::execute(*r.program, d, b.px.data(), 256u * 4u);
  return b;
}

nsl::Texture texture_of(const Bitmap& b) {
  nsl::Texture t;
  t.bgra = b.px.data();
  t.w = b.w;
  t.h = b.h;
  t.stride = static_cast<size_t>(b.w) * 4u;
  return t;
}

double now_s() {
  return std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count();
}

// Best of `reps` runs, in ns per pixel.
double time_ns_per_px(const nsl::Program& p, const nsl::Draw& d, bool simd, int reps) {
  const nsl::Placement pl = nsl::placement(d);
  std::vector<uint8_t> px(static_cast<size_t>(pl.w) * static_cast<size_t>(pl.h) * 4u);
  const size_t stride = static_cast<size_t>(pl.w) * 4u;
  double best = 1e30;
  for (int r = 0; r <= reps; ++r) {  // the first run warms up
    const double t0 = now_s();
    if (simd) nsl::execute(p, d, px.data(), stride);
    else nsl::execute_scalar(p, d, px.data(), stride);
    if (r > 0) best = std::min(best, now_s() - t0);
  }
  return best * 1e9 / (static_cast<double>(pl.w) * static_cast<double>(pl.h));
}

}  // namespace

int main() {
#if defined(_MSC_VER) && !defined(__clang__)
  std::printf("compiler: MSVC %d\n", _MSC_VER);
#elif defined(__clang__)
  std::printf("compiler: clang %s\n", __clang_version__);
#elif defined(__GNUC__)
  std::printf("compiler: GCC %s\n", __VERSION__);
#endif
  const Bitmap bitmaps[] = {Bitmap(), photo(), normal_map(), noise_tile()};
  std::printf("\n%-11s %5s %5s %5s %5s %5s %8s %8s %12s %14s %13s\n", "shader", "pixel", "draw", "trans", "fetch",
              "regs", "scalar", "SIMD", "knob 120@2x", "bg 800x600@2x", "bg @ res 0.5");
  std::printf("%-11s %5s %5s %5s %5s %5s %8s %8s %12s %14s %13s\n", "", "ops", "ops", "", "", "", "ns/px", "ns/px",
              "ms", "ms", "ms");
  for (const Shader& s : k_shaders) {
    const nsl::CompileResult r = nsl::compile(s.src, std::strlen(s.src), s.name);
    if (!r.program) {
      std::printf("%s\n", r.error.c_str());
      return 1;
    }
    const nsl::Program& p = *r.program;
    const nsl::Stats st = nsl::stats(p);
    const nsl::Texture tex = texture_of(bitmaps[s.tex]);
    nsl::Draw knob;
    if (s.tex != TEX_NONE) {
      knob.textures = &tex;
      knob.texture_count = 1;
    }
    knob.w = knob.h = 120.0f;
    knob.scale = 2.0f;
    knob.time = 1.7f;
    nsl::Draw bg = knob;
    bg.w = 800.0f;
    bg.h = 600.0f;
    nsl::Draw bg_half = bg;
    bg_half.scale = 1.0f;
    const double scalar = time_ns_per_px(p, knob, false, 3);
    const double simd = time_ns_per_px(p, knob, true, 7);
    const double bg_ns = time_ns_per_px(p, bg, true, 2);
    const double half_ns = time_ns_per_px(p, bg_half, true, 3);
    std::printf("%-11s %5d %5d %5d %5d %5d %8.2f %8.2f %12.2f %14.1f %13.1f\n", s.name, st.pixel_ops, st.draw_ops,
                st.transcendentals, st.fetches, st.pixel_regs, scalar, simd, simd * 240.0 * 240.0 * 1e-6,
                bg_ns * 1600.0 * 1200.0 * 1e-6, half_ns * 800.0 * 600.0 * 1e-6);
  }
  return 0;
}
