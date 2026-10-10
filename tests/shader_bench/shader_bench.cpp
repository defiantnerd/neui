// Shader executor benchmark: compiles a few .nsl programs with the real
// compiler (src/shader/nsl.h) and times execute() at widget and background
// sizes. Report only; correctness and the cross-platform goldens live in
// neui_tests (test_shader_exec.cpp).
//
//   neui_shader_bench

#include "shader/nsl.h"

#include <algorithm>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <vector>

namespace {

namespace nsl = neui::nsl;

struct Shader {
  const char* name;
  const char* src;
};

const Shader k_shaders[] = {
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
};

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
  std::printf("\n%-11s %5s %5s %5s %5s %8s %8s %12s %14s %13s\n", "shader", "pixel", "draw", "trans", "regs",
              "scalar", "SIMD", "knob 120@2x", "bg 800x600@2x", "bg @ res 0.5");
  std::printf("%-11s %5s %5s %5s %5s %8s %8s %12s %14s %13s\n", "", "ops", "ops", "", "", "ns/px", "ns/px", "ms", "ms",
              "ms");
  for (const Shader& s : k_shaders) {
    const nsl::CompileResult r = nsl::compile(s.src, std::strlen(s.src), s.name);
    if (!r.program) {
      std::printf("%s\n", r.error.c_str());
      return 1;
    }
    const nsl::Program& p = *r.program;
    const nsl::Stats st = nsl::stats(p);
    nsl::Draw knob;
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
    std::printf("%-11s %5d %5d %5d %5d %8.2f %8.2f %12.2f %14.1f %13.1f\n", s.name, st.pixel_ops, st.draw_ops,
                st.transcendentals, st.pixel_regs, scalar, simd, simd * 240.0 * 240.0 * 1e-6,
                bg_ns * 1600.0 * 1200.0 * 1e-6, half_ns * 800.0 * 600.0 * 1e-6);
  }
  return 0;
}
