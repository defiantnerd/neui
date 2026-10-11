// Windows-only: runs .nsl programs on the GPU through the real D2D backend
// (create_shader_program / draw_shader_program into an off-screen context)
// and holds them against the CPU executor with the fidelity contract of
// docs/shader-language.md section 12: per channel within 2/255 on at least
// 99.9% of pixels, and within 8/255 on all of them.
//
// Also covers what only a real D2D device can show: two draws of one program
// in one frame (the effect is reused between them), a pushed clip, the alpha
// stack, and nothing drawn outside the placement. Without d3dcompiler_47.dll
// or a feature level 10_0 device it reports SKIP and passes; with them, a
// program that does not compile fails.
//
// Exit code 0 = pass (or skip), 1 = fail. Prints a table per case.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d3d11.h>

#include <neui/neui.h>

#include "d2d_backend.h"
#include "shader/nsl.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <string>
#include <vector>

#pragma comment(lib, "d3d11")

namespace nsl = neui::nsl;

namespace {

struct Src { const char* name; const char* text; };

const Src k_corpus[] = {
  { "ring_glow", R"(
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
})" },
  { "plasma", R"(
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
})" },
  { "fbm", R"(
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
})" },
  { "synthetic", R"(
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
})" },
  { "dither", R"(
#pragma neui dither
vec4 main(vec2 p) {
  float g = p.x / neui_size.x;
  return vec4(g, g * 0.5, 1.0 - g, 0.4 + 0.6 * p.y / neui_size.y);
})" },
  { "knob", R"(
uniform float value = 0.37;
uniform vec4  accent = vec4(0.25, 0.66, 0.96, 1.0);
vec4 over(vec4 top, vec4 bottom) {
  float a = top.a + bottom.a * (1.0 - top.a);
  vec3 c = (top.rgb * top.a + bottom.rgb * (bottom.a * (1.0 - top.a))) / max(a, 1e-6);
  return vec4(c, a);
}
vec4 main(vec2 p) {
  vec2  c  = neui_size * 0.5;
  float R  = min(c.x, c.y);
  vec2  d  = p - c;
  float r  = length(d);
  float aa = neui_px;
  float ang   = atan(d.x, -d.y);
  float sweep = -2.3561945 + value * 4.712389;
  float range = 1.0 - step(2.3561945, abs(ang));
  float lit   = range * (1.0 - step(sweep, ang));
  float ring_d = abs(r - R * 0.80) - R * 0.065;
  float ring   = 1.0 - smoothstep(-aa, aa, ring_d);
  float glow   = exp(-max(ring_d, 0.0) / (R * 0.07)) * lit * 0.55;
  vec4 col = vec4(accent.rgb, glow * accent.a);
  col = over(vec4(mix(vec3(0.17, 0.19, 0.23), accent.rgb, lit), ring * range), col);
  float body_r = R * 0.62;
  vec3  body   = mix(vec3(0.22, 0.24, 0.29), vec3(0.08, 0.09, 0.11), d.y / body_r * 0.5 + 0.5);
  col = over(vec4(body, 1.0 - smoothstep(-aa, aa, r - body_r)), col);
  vec2  tip = c + vec2(sin(sweep), -cos(sweep)) * (body_r * 0.70);
  return over(vec4(accent.rgb, 1.0 - smoothstep(-aa, aa, length(p - tip) - R * 0.07)), col);
})" },
};

// Programs that sample bitmaps (plans/shader-effects.md, phase 9).
const Src k_textured[] = {
  { "tex_copy", R"(
uniform sampler2D img;
vec4 main(vec2 p) { return texture2D(img, p / neui_size); })" },
  { "tex_nearest", R"(
#pragma neui sampler img nearest
uniform sampler2D img;
vec4 main(vec2 p) { return texture2D(img, p / neui_size); })" },
  { "tex_repeat", R"(
#pragma neui sampler img repeat
uniform sampler2D img;
vec4 main(vec2 p) {
  vec2 uv = (p - neui_size * 0.5) / neui_size * 1.6 + 0.5;
  return mix(texture2D(img, uv), texture2D(img, uv.yx * 3.0), 0.25);
})" },
  { "tex_border_two", R"(
#pragma neui sampler a border nearest
uniform sampler2D a;
uniform sampler2D b;
vec4 main(vec2 p) {
  vec2 uv = (p / neui_size - 0.25) * 2.0;
  vec4 lit = texture2D(b, p / neui_size * 0.5 + neui_texel(b) * 0.5);
  return texture2D(a, uv) * 0.5 + lit * vec4(0.5, 0.25, 0.75, 0.5);
})" },
};

// A deterministic premultiplied BGRA bitmap with transparent texels.
struct Bitmap {
  uint32_t w = 0, h = 0;
  std::vector<uint8_t> px;
};

Bitmap make_bitmap(uint32_t w, uint32_t h, uint32_t seed)
{
  Bitmap b;
  b.w = w;
  b.h = h;
  b.px.resize(size_t(w) * h * 4);
  uint32_t s = seed;
  for (size_t i = 0; i < b.px.size(); i += 4) {
    s = s * 1664525u + 1013904223u;
    const uint32_t a = (s >> 24) % 5u == 0 ? 0u : (s >> 24);
    for (int c = 0; c < 3; ++c) {
      s = s * 1664525u + 1013904223u;
      b.px[i + size_t(c)] = uint8_t((s >> 16) % (a + 1u));
    }
    b.px[i + 3] = uint8_t(a);
  }
  return b;
}

neui_shader_texture_desc_t desc_of(const Bitmap& b, uint64_t id)
{
  neui_shader_texture_desc_t d = {};
  d.bgra = b.px.data();
  d.width_px = b.w;
  d.height_px = b.h;
  d.stride = size_t(b.w) * 4;
  d.content_id = id;
  return d;
}

nsl::Texture texture_of(const neui_shader_texture_desc_t& d, float scale = 1.0f)
{
  nsl::Texture t;
  t.bgra = d.bgra;
  t.w = int(d.width_px);
  t.h = int(d.height_px);
  t.stride = d.stride;
  t.scale = scale;
  return t;
}

neui_render_backend_t* g_be = nullptr;
int g_failures = 0;

struct Stats {
  long total = 0, over2 = 0, spill = 0;
  int maxd = 0;
  void add(const Stats& o) { total += o.total; over2 += o.over2; spill += o.spill; maxd = std::max(maxd, o.maxd); }
  double pct() const { return total ? 100.0 * double(over2) / double(total) : 0.0; }
  bool ok() const { return maxd <= 8 && pct() <= 0.1 && spill == 0; }
};

// GPU target pixels vs a CPU image at the placement; with `check_rest`, the
// rest of the target must stay transparent.
Stats compare(const std::vector<uint8_t>& target, uint32_t tw, uint32_t th,
              const std::vector<uint8_t>& cpu, const nsl::Placement& pl, bool check_rest = true)
{
  Stats s;
  for (uint32_t y = 0; y < th; ++y)
    for (uint32_t x = 0; x < tw; ++x) {
      const uint8_t* g = &target[(size_t(y) * tw + x) * 4];
      const int lx = int(x) - pl.x, ly = int(y) - pl.y;
      if (lx < 0 || ly < 0 || lx >= pl.w || ly >= pl.h) {
        if (check_rest && (g[0] | g[1] | g[2] | g[3])) ++s.spill;
        continue;
      }
      const uint8_t* c = &cpu[(size_t(ly) * size_t(pl.w) + size_t(lx)) * 4];
      int d = 0;
      for (int k = 0; k < 4; ++k) d = std::max(d, std::abs(int(g[k]) - int(c[k])));
      ++s.total;
      if (d > 2) ++s.over2;
      s.maxd = std::max(s.maxd, d);
    }
  return s;
}

struct Case {
  std::shared_ptr<const nsl::Program> prog;
  void* gpu = nullptr;
  std::vector<neui_shader_texture_desc_t> tex;  // a program that samples: its bitmaps
};

std::vector<uint8_t> cpu_image(const nsl::Program& p, const nsl::Draw& d, nsl::Placement& pl)
{
  pl = nsl::placement(d);
  std::vector<uint8_t> px(size_t(pl.w) * size_t(pl.h) * 4);
  nsl::execute(p, d, px.data(), size_t(pl.w) * 4);
  return px;
}

bool gpu_draw(neui_render_ctx_t ctx, const Case& c, const nsl::Draw& d)
{
  const nsl::Placement pl = nsl::placement(d);
  std::vector<float> cb(nsl::gpu_constant_count(*c.prog));
  if (!nsl::gpu_constants(*c.prog, d, cb.data())) return false;
  if (!c.tex.empty())
    return g_be->draw_shader_program_textured(ctx, c.gpu, pl.x, pl.y, uint32_t(pl.w), uint32_t(pl.h), cb.data(),
                                              uint32_t(cb.size()), c.tex.data(), uint32_t(c.tex.size()));
  return g_be->draw_shader_program(ctx, c.gpu, pl.x, pl.y, uint32_t(pl.w), uint32_t(pl.h), cb.data(),
                                   uint32_t(cb.size()));
}

// Programs compile on a worker thread and decline draws until they are
// ready: draw a 1 x 1 px frame until one is accepted. False after 20 s.
bool wait_ready(const Case& c)
{
  neui_render_ctx_t ctx = g_be->create_offscreen_context(4, 4, 1.0f);
  nsl::Draw d;
  d.w = d.h = 1.0f;
  bool ok = false;
  for (int i = 0; i < 4000 && !ok; ++i) {
    g_be->begin_frame(ctx, 0x00000000u);
    ok = gpu_draw(ctx, c, d);
    g_be->end_frame(ctx);
    if (!ok) Sleep(5);
  }
  g_be->destroy_context(ctx);
  return ok;
}

void report(const char* what, const Stats& s)
{
  std::printf("| %-34s | %7ld | %3d | %6.3f%% | %5ld | %s |\n", what, s.total, s.maxd, s.pct(), s.spill,
              s.ok() ? "ok" : "FAIL");
  if (!s.ok()) ++g_failures;
}

bool prerequisites(std::string& why)
{
  if (!LoadLibraryExW(L"d3dcompiler_47.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32)) {
    why = "d3dcompiler_47.dll not found";
    return false;
  }
  const D3D_DRIVER_TYPE types[] = { D3D_DRIVER_TYPE_HARDWARE, D3D_DRIVER_TYPE_WARP };
  for (D3D_DRIVER_TYPE type : types) {
    ID3D11Device* dev = nullptr;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_9_1;
    if (SUCCEEDED(D3D11CreateDevice(nullptr, type, nullptr, D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                    D3D11_SDK_VERSION, &dev, &fl, nullptr))) {
      dev->Release();
      if (fl >= D3D_FEATURE_LEVEL_10_0) return true;
      why = "feature level below 10_0";
      return false;
    }
  }
  why = "no D3D11 device";
  return false;
}

}  // namespace

int main()
{
  std::string why;
  if (!prerequisites(why)) {
    std::printf("SKIP: %s\n", why.c_str());
    return 0;
  }
  g_be = neui_d2d_backend::get_backend();
  if (!g_be->create_shader_program || !g_be->draw_shader_program || !g_be->create_offscreen_context) {
    std::printf("FAIL: the D2D backend has no shader program slots\n");
    return 1;
  }

  // Build the device first, as a window would have, so the compile times
  // below do not include it.
  if (neui_render_ctx_t warm = g_be->create_offscreen_context(4, 4, 1.0f)) g_be->destroy_context(warm);

  std::vector<Case> cases;
  for (const Src& s : k_corpus) {
    nsl::CompileResult r = nsl::compile(s.text, std::strlen(s.text), s.name);
    if (!r.program) { std::printf("FAIL: %s\n", r.error.c_str()); return 1; }
    Case c;
    c.prog = r.program;
    const std::string hlsl = nsl::emit_hlsl(*c.prog);
    const auto t0 = std::chrono::steady_clock::now();
    c.gpu = g_be->create_shader_program(NEUI_SHADER_LANG_HLSL_D2D, hlsl.data(), hlsl.size());
    const double create_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (!c.gpu || !wait_ready(c)) {
      std::printf("FAIL: %s did not compile for the GPU (see the debug output)\n", s.name);
      return 1;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("compiled %-10s %6.1f ms (create returned in %.2f ms), %zu constants, %zu bytes of HLSL\n",
                s.name, ms, create_ms, nsl::gpu_constant_count(*c.prog), hlsl.size());
    cases.push_back(c);
  }

  // Programs that sample, each with its bitmaps (ids 1 and 2).
  const Bitmap bm_a = make_bitmap(13, 9, 0x9E3779B9u), bm_b = make_bitmap(37, 23, 12345u);
  std::vector<Case> textured;
  if (!g_be->draw_shader_program_textured) {
    std::printf("FAIL: the D2D backend has no draw_shader_program_textured\n");
    return 1;
  }
  for (const Src& s : k_textured) {
    nsl::CompileResult r = nsl::compile(s.text, std::strlen(s.text), s.name);
    if (!r.program) { std::printf("FAIL: %s\n", r.error.c_str()); return 1; }
    Case c;
    c.prog = r.program;
    c.tex.push_back(desc_of(bm_a, 1));
    if (nsl::samplers(*c.prog).size() > 1) c.tex.push_back(desc_of(bm_b, 2));
    const std::string hlsl = nsl::emit_hlsl(*c.prog);
    const auto t0 = std::chrono::steady_clock::now();
    c.gpu = g_be->create_shader_program(NEUI_SHADER_LANG_HLSL_D2D, hlsl.data(), hlsl.size());
    if (!c.gpu || !wait_ready(c)) {
      std::printf("FAIL: %s did not compile or draw on the GPU (see the debug output)\n%s\n", s.name, hlsl.c_str());
      return 1;
    }
    const double ms = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    std::printf("compiled %-14s %6.1f ms, %zu sampler(s), %zu bytes of HLSL\n", s.name, ms,
                nsl::samplers(*c.prog).size(), hlsl.size());
    textured.push_back(c);
  }

  std::printf("\n| case                               |  pixels | max | > 2/255 | spill | result |\n");
  std::printf("|---|---|---|---|---|---|\n");

  const float scales[] = { 1.0f, 1.25f, 1.5f, 2.0f };
  const float origins[][2] = { { 3.0f, 2.0f }, { 3.5f, 2.25f }, { 2.2f, 1.7f } };
  const float lw = 96.0f, lh = 64.0f;
  for (float s : scales) {
    const uint32_t tw = uint32_t(std::ceil(110.0f * s)), th = uint32_t(std::ceil(80.0f * s));
    neui_render_ctx_t ctx = g_be->create_offscreen_context(tw, th, s);
    if (!ctx) { std::printf("FAIL: no offscreen context at %.2fx\n", double(s)); return 1; }
    std::vector<uint8_t> target(size_t(tw) * th * 4);
    for (size_t k = 0; k < cases.size(); ++k) {
      Stats all;
      for (const auto& o : origins) {
        nsl::Draw d;
        d.x = o[0]; d.y = o[1]; d.w = lw; d.h = lh; d.scale = s; d.time = 1.3f;
        nsl::Placement pl;
        const std::vector<uint8_t> cpu = cpu_image(*cases[k].prog, d, pl);
        g_be->begin_frame(ctx, 0x00000000u);
        const bool drew = gpu_draw(ctx, cases[k], d);
        g_be->end_frame(ctx);
        g_be->read_pixels_bgra(ctx, target.data());
        if (!drew) { std::printf("FAIL: %s not drawn\n", k_corpus[k].name); ++g_failures; continue; }
        all.add(compare(target, tw, th, cpu, pl));
      }
      char what[64];
      std::snprintf(what, sizeof what, "%s @%.2fx", k_corpus[k].name, double(s));
      report(what, all);
    }

    // Two draws of one program in one frame, different uniforms and places.
    {
      const Case& c = cases[0];  // ring_glow
      const float v1 = 0.2f, v2 = 0.9f;
      float u1[5] = { v1, 0.2f, 0.7f, 1.0f, 1.0f }, u2[5] = { v2, 1.0f, 0.3f, 0.1f, 1.0f };
      nsl::Draw a, b;
      a.x = 1.0f; a.y = 1.0f; a.w = 48.0f; a.h = 48.0f; a.scale = s; a.uniforms = u1;
      b = a; b.x = 55.5f; b.uniforms = u2;
      nsl::Placement pa, pb;
      const std::vector<uint8_t> ca = cpu_image(*c.prog, a, pa), cbm = cpu_image(*c.prog, b, pb);
      g_be->begin_frame(ctx, 0x00000000u);
      gpu_draw(ctx, c, a);
      gpu_draw(ctx, c, b);
      g_be->end_frame(ctx);
      g_be->read_pixels_bgra(ctx, target.data());
      Stats st = compare(target, tw, th, ca, pa, false);
      st.add(compare(target, tw, th, cbm, pb, false));
      char what[64];
      std::snprintf(what, sizeof what, "two draws, one frame @%.2fx", double(s));
      report(what, st);
    }

    // A pushed clip: only its inside is drawn.
    {
      const Case& c = cases[1];  // plasma
      nsl::Draw d;
      d.x = 4.0f; d.y = 4.0f; d.w = lw; d.h = lh; d.scale = s;
      nsl::Placement pl;
      std::vector<uint8_t> cpu = cpu_image(*c.prog, d, pl);
      // Logical, on whole device px at every scale tested (no pixel centre on an edge).
      const float cx = 20.0f, cy = 8.0f, cw = 40.0f, ch = 32.0f;
      for (int y = 0; y < pl.h; ++y)
        for (int x = 0; x < pl.w; ++x) {
          const float gx = float(pl.x + x) + 0.5f, gy = float(pl.y + y) + 0.5f;
          if (gx < cx * s || gx > (cx + cw) * s || gy < cy * s || gy > (cy + ch) * s)
            std::memset(&cpu[(size_t(y) * size_t(pl.w) + size_t(x)) * 4], 0, 4);
        }
      g_be->begin_frame(ctx, 0x00000000u);
      g_be->push_clip(ctx, cx, cy, cw, ch);
      gpu_draw(ctx, c, d);
      g_be->pop_clip(ctx);
      g_be->end_frame(ctx);
      g_be->read_pixels_bgra(ctx, target.data());
      char what[64];
      std::snprintf(what, sizeof what, "pushed clip @%.2fx", double(s));
      report(what, compare(target, tw, th, cpu, pl));
    }

    // The alpha stack: half opacity.
    {
      const Case& c = cases[2];  // fbm
      nsl::Draw d;
      d.x = 4.0f; d.y = 4.0f; d.w = lw; d.h = lh; d.scale = s;
      nsl::Placement pl;
      std::vector<uint8_t> cpu = cpu_image(*c.prog, d, pl);
      for (uint8_t& v : cpu) v = uint8_t(std::lround(v * 0.5));
      g_be->begin_frame(ctx, 0x00000000u);
      g_be->push_alpha(ctx, 0.5f);
      gpu_draw(ctx, c, d);
      g_be->pop_alpha(ctx);
      g_be->end_frame(ctx);
      g_be->read_pixels_bgra(ctx, target.data());
      char what[64];
      std::snprintf(what, sizeof what, "alpha 0.5 @%.2fx", double(s));
      report(what, compare(target, tw, th, cpu, pl));
    }
    // Programs that sample, at every origin.
    for (size_t k = 0; k < textured.size(); ++k) {
      const Case& c = textured[k];
      std::vector<nsl::Texture> tx;
      for (const auto& d : c.tex) tx.push_back(texture_of(d));
      Stats all;
      for (const auto& o : origins) {
        nsl::Draw d;
        d.x = o[0]; d.y = o[1]; d.w = lw; d.h = lh; d.scale = s;
        d.textures = tx.data();
        d.texture_count = int(tx.size());
        nsl::Placement pl;
        const std::vector<uint8_t> cpu = cpu_image(*c.prog, d, pl);
        g_be->begin_frame(ctx, 0x00000000u);
        const bool drew = gpu_draw(ctx, c, d);
        g_be->end_frame(ctx);
        g_be->read_pixels_bgra(ctx, target.data());
        if (!drew) { std::printf("FAIL: %s not drawn\n", k_textured[k].name); ++g_failures; continue; }
        all.add(compare(target, tw, th, cpu, pl));
      }
      char what[64];
      std::snprintf(what, sizeof what, "%s @%.2fx", k_textured[k].name, double(s));
      report(what, all);
    }

    // A bitmap drawn 1:1 at its own scale, on whole device px: exact on the
    // CPU, and should be on the GPU too (every weight is 0 or 256).
    {
      const Case& c = textured[0];  // tex_copy
      const Bitmap big = make_bitmap(uint32_t(std::lround(80.0f * s)), uint32_t(std::lround(60.0f * s)), 777u);
      Case one = c;
      one.tex = { desc_of(big, 10 + uint64_t(s * 100.0f)) };
      const nsl::Texture t = texture_of(one.tex[0], s);
      nsl::Draw d;
      d.x = 4.0f; d.y = 4.0f; d.w = 80.0f; d.h = 60.0f; d.scale = s;
      d.textures = &t;
      d.texture_count = 1;
      nsl::Placement pl;
      const std::vector<uint8_t> cpu = cpu_image(*c.prog, d, pl);
      g_be->begin_frame(ctx, 0x00000000u);
      gpu_draw(ctx, one, d);
      g_be->end_frame(ctx);
      g_be->read_pixels_bgra(ctx, target.data());
      const Stats st = compare(target, tw, th, cpu, pl);
      char what[64];
      std::snprintf(what, sizeof what, "copy 1:1, max should be 0 @%.2fx", double(s));
      report(what, st);
      if (cpu != big.px) { std::printf("FAIL: the CPU copy is not exact\n"); ++g_failures; }
    }

    // New pixels under a new id are drawn; unbound samplers read transparent.
    {
      const Case& c = textured[0];
      Bitmap changed = bm_a;
      for (size_t i = 0; i < changed.px.size(); i += 4) changed.px[i + 3] = changed.px[i + 1] = 255;
      Case next = c;
      next.tex = { desc_of(changed, 3) };
      Case none = c;
      none.tex = { neui_shader_texture_desc_t{} };
      const nsl::Texture t = texture_of(next.tex[0]);
      nsl::Draw d;
      d.x = 4.0f; d.y = 4.0f; d.w = lw; d.h = lh; d.scale = s;
      d.textures = &t;
      d.texture_count = 1;
      nsl::Placement pl;
      const std::vector<uint8_t> cpu = cpu_image(*c.prog, d, pl);
      g_be->begin_frame(ctx, 0x00000000u);
      gpu_draw(ctx, c, d);     // the old pixels first, in the same frame
      g_be->end_frame(ctx);
      g_be->begin_frame(ctx, 0x00000000u);
      gpu_draw(ctx, next, d);
      g_be->end_frame(ctx);
      g_be->read_pixels_bgra(ctx, target.data());
      char what[64];
      std::snprintf(what, sizeof what, "new content id @%.2fx", double(s));
      report(what, compare(target, tw, th, cpu, pl));

      // Two draws of one program in one frame, each with its own bitmap.
      {
        const nsl::Texture ta = texture_of(c.tex[0]);
        nsl::Draw a = d, b = d;
        a.w = b.w = 48.0f;
        a.textures = &ta;
        b.x = 55.5f;
        nsl::Placement pa, pb;
        const std::vector<uint8_t> ca = cpu_image(*c.prog, a, pa), cbm = cpu_image(*c.prog, b, pb);
        g_be->begin_frame(ctx, 0x00000000u);
        gpu_draw(ctx, c, a);
        gpu_draw(ctx, next, b);
        g_be->end_frame(ctx);
        g_be->read_pixels_bgra(ctx, target.data());
        Stats st = compare(target, tw, th, ca, pa, false);
        st.add(compare(target, tw, th, cbm, pb, false));
        std::snprintf(what, sizeof what, "two bitmaps, one frame @%.2fx", double(s));
        report(what, st);
      }

      d.textures = nullptr;
      d.texture_count = 0;
      const std::vector<uint8_t> clear = cpu_image(*c.prog, d, pl);
      g_be->begin_frame(ctx, 0x00000000u);
      gpu_draw(ctx, none, d);
      g_be->end_frame(ctx);
      g_be->read_pixels_bgra(ctx, target.data());
      std::snprintf(what, sizeof what, "unbound sampler @%.2fx", double(s));
      report(what, compare(target, tw, th, clear, pl));
    }
    g_be->destroy_context(ctx);
  }

  // Timing, report only: a 240 x 240 px bilinear copy per frame, GPU vs CPU.
  {
    neui_render_ctx_t ctx = g_be->create_offscreen_context(240, 240, 1.0f);
    std::vector<uint8_t> target(240 * 240 * 4);
    const Case& c = textured[0];
    const nsl::Texture t = texture_of(c.tex[0]);
    nsl::Draw d;
    d.w = d.h = 240.0f;
    d.textures = &t;
    d.texture_count = 1;
    const int frames = 200;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
      g_be->begin_frame(ctx, 0x00000000u);
      gpu_draw(ctx, c, d);
      g_be->end_frame(ctx);
    }
    g_be->read_pixels_bgra(ctx, target.data());
    const double gpu_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
    std::vector<uint8_t> px(240 * 240 * 4);
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) nsl::execute(*c.prog, d, px.data(), 240 * 4);
    const double cpu_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count() / 20;
    std::printf("\ntex_copy 240x240 px per frame: GPU %.3f ms, CPU %.3f ms\n", gpu_ms, cpu_ms);
    g_be->destroy_context(ctx);
  }

  // Timing, report only: a 240 x 240 px plasma per frame, GPU vs CPU.
  {
    neui_render_ctx_t ctx = g_be->create_offscreen_context(240, 240, 1.0f);
    std::vector<uint8_t> target(240 * 240 * 4);
    nsl::Draw d;
    d.w = d.h = 240.0f;
    const int frames = 200;
    const auto t0 = std::chrono::steady_clock::now();
    for (int i = 0; i < frames; ++i) {
      g_be->begin_frame(ctx, 0x00000000u);
      gpu_draw(ctx, cases[1], d);
      g_be->end_frame(ctx);
    }
    g_be->read_pixels_bgra(ctx, target.data());  // waits for the GPU
    const double gpu_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count() / frames;
    std::vector<uint8_t> px(240 * 240 * 4);
    const auto t1 = std::chrono::steady_clock::now();
    for (int i = 0; i < 20; ++i) nsl::execute(*cases[1].prog, d, px.data(), 240 * 4);
    const double cpu_ms =
      std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t1).count() / 20;
    std::printf("\nplasma 240x240 px per frame: GPU %.3f ms, CPU %.3f ms\n", gpu_ms, cpu_ms);
    g_be->destroy_context(ctx);
  }

  for (Case& c : cases) g_be->destroy_shader_program(c.gpu);
  for (Case& c : textured) g_be->destroy_shader_program(c.gpu);
  std::printf("\n%s (%d failing case%s)\n", g_failures ? "FAIL" : "PASS", g_failures, g_failures == 1 ? "" : "s");
  return g_failures ? 1 : 0;
}
