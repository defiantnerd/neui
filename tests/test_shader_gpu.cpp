#include "neui_test.h"

// The GPU form of .nsl programs (src/shader/nsl_gpu.cpp: emit_hlsl,
// gpu_constants) and the host's choice between it and the CPU path
// (hosts/shared/shader_layer.h shader_paint, ShaderCache::gpu_program), with
// a fake backend that records shader-program calls. Whether the HLSL runs and
// matches the CPU is checked on Windows by tests/shader_gpu_smoke.cpp.

#include "asset_store.h"
#include "painter.h"
#include "shader_api.h"
#include "shader_layer.h"

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace neui_detail;
namespace nsl = neui::nsl;

namespace {

std::shared_ptr<const nsl::Program> build(const char* src)
{
  nsl::CompileResult r = nsl::compile(src, std::strlen(src), "gpu.nsl");
  if (!r.program) std::fprintf(stderr, "  compile error: %s\n", r.error.c_str());
  return r.program;
}

bool has(const std::string& s, const char* what) { return s.find(what) != std::string::npos; }

uint32_t crc32(const std::string& s)
{
  uint32_t crc = ~0u;
  for (unsigned char ch : s) {
    crc ^= ch;
    for (int j = 0; j < 8; ++j) crc = (crc & 1u) ? 0xEDB88320u ^ (crc >> 1) : crc >> 1;
  }
  return ~crc;
}

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

const char* const k_everything = R"(
#pragma neui dither
uniform float k = 2.0;
vec4 main(vec2 p) {
  float a = sin(p.x) + cos(p.y) + exp2(p.x * 0.01) + log2(p.y + 1.0) + atan(p.x, p.y);
  float n = neui_noise(p * 0.1) + neui_hash(p);
  return vec4(fract(a), n * 0.5, k * 3.0, step(0.5, fract(p.x)));
})";

const char* const k_flat = "vec4 main(vec2 p) { return vec4(p.x / neui_size.x, 0.5, 0.25, 1.0); }";

const char* const k_cpu_only = "#pragma neui cpu_only\nvec4 main(vec2 p) { return vec4(1.0); }";

}  // namespace

// ---- The emitter ---------------------------------------------------------------

TEST_CASE("shader gpu: emit_hlsl writes a D2D effect pixel shader")
{
  auto prog = build(k_ring_glow);
  REQUIRE(prog != nullptr);
  const std::string h = nsl::emit_hlsl(*prog);
  REQUIRE(!h.empty());
  CHECK(has(h, "float4 main(float4 pos : SV_POSITION, float4 scene : SCENE_POSITION) : SV_Target"));
  CHECK(has(h, "cbuffer C : register(b0) { float4 c["));
  const size_t n = nsl::gpu_constant_count(*prog);
  CHECK(n >= 8u);
  CHECK_EQ(n % 4, 0u);
  CHECK(has(h, ("float4 c[" + std::to_string(n / 4) + "]").c_str()));
  CHECK(has(h, "n_exp2"));          // exp() lowers to exp2
  CHECK_FALSE(has(h, "n_sin"));     // helpers only when used
  CHECK_FALSE(has(h, "n_hash2"));
  CHECK_FALSE(has(h, "n_bayer"));   // no dither pragma
  CHECK(has(h, "precise float"));
  CHECK(nsl::emit_hlsl(*prog) == h);  // deterministic
}

TEST_CASE("shader gpu: every op has an HLSL form")
{
  auto prog = build(k_everything);
  REQUIRE(prog != nullptr);
  const std::string h = nsl::emit_hlsl(*prog);
  REQUIRE(!h.empty());
  for (const char* f : { "n_sin(", "n_cos(", "n_exp2(", "n_log2(", "n_atan2(", "n_hash2(", "n_bayer[" })
    CHECK(has(h, f));
  // No GPU intrinsic stands in for neui's math.
  for (const char* f : { " sin(", " cos(", " exp2(", " log2(", " atan2(", " exp(", " log(" })
    CHECK_FALSE(has(h, f));
}

TEST_CASE("shader gpu: cpu_only programs have no GPU form")
{
  auto prog = build(k_cpu_only);
  REQUIRE(prog != nullptr);
  CHECK(nsl::emit_hlsl(*prog).empty());
  CHECK_EQ(nsl::gpu_constant_count(*prog), 0u);
  nsl::Draw d;
  d.w = d.h = 8.0f;
  float buf[8] = {};
  CHECK_FALSE(nsl::gpu_constants(*prog, d, buf));
}

TEST_CASE("shader gpu: the emitted text is pinned")
{
  // Text goldens: the compiler numbers its IR canonically, so the HLSL is
  // the same with every C++ compiler and standard library. Update them when
  // the emitter or the compiler's output changes on purpose, and run the
  // Windows smoke test.
  auto a = build(k_ring_glow), b = build(k_everything);
  REQUIRE(a != nullptr);
  REQUIRE(b != nullptr);
  CHECK_EQ(crc32(nsl::emit_hlsl(*a)), 0x13f193cfu);
  CHECK_EQ(crc32(nsl::emit_hlsl(*b)), 0x39d023edu);
}

// ---- The constants -------------------------------------------------------------

TEST_CASE("shader gpu: constants carry the pixel grid and the draw values")
{
  auto prog = build(k_everything);
  REQUIRE(prog != nullptr);
  const size_t n = nsl::gpu_constant_count(*prog);
  std::vector<float> c(n, -1.0f);
  nsl::Draw d;
  d.x = 10.25f / 1.5f;  // device origin 10.25
  d.y = 3.0f;           // device origin 4.5
  d.w = 20.0f;
  d.h = 10.0f;
  d.scale = 1.5f;
  REQUIRE(nsl::gpu_constants(*prog, d, c.data()));
  const nsl::Placement pl = nsl::placement(d);
  CHECK_EQ(pl.x, 10);
  CHECK_EQ(pl.y, 4);
  CHECK_APPROX(c[0], 0.25);                    // fx
  CHECK_APPROX(c[1], 0.5);                     // fy
  CHECK_APPROX(c[2], 0.25 + 30.0);             // X1, from the floored origin
  CHECK_APPROX(c[3], 0.5 + 15.0);              // Y1
  CHECK_EQ(c[4], 1.5f);                        // scale
  CHECK_EQ(c[nsl::k_gpu_opacity], 1.0f);
  CHECK_EQ(nsl::k_gpu_opacity, size_t(NEUI_SHADER_OPACITY_INDEX));
  // k * 3.0 is draw-time work: its value travels in the buffer.
  bool six = false;
  for (size_t i = 8; i < n; ++i) six = six || c[i] == 6.0f;
  CHECK(six);
  const float k5 = 5.0f;
  d.uniforms = &k5;
  REQUIRE(nsl::gpu_constants(*prog, d, c.data()));
  bool fifteen = false;
  for (size_t i = 8; i < n; ++i) fifteen = fifteen || c[i] == 15.0f;
  CHECK(fifteen);
  // An empty placement writes nothing.
  d.w = 0.0f;
  CHECK_FALSE(nsl::gpu_constants(*prog, d, c.data()));
}

// ---- The host's choice ----------------------------------------------------------

namespace {

struct GpuBackend {
  int creates = 0, destroys = 0, gpu_draws = 0, bitmaps = 0;
  bool create_ok = true, draw_ok = true;
  std::string last_src;
  int32_t x = 0, y = 0;
  uint32_t w = 0, h = 0;
  std::vector<float> constants;
  float m[6] = { 1, 0, 0, 1, 0, 0 };
  neui_render_backend_t table{};
};
GpuBackend* g_gb = nullptr;

void* NEUI_ABI gb_create_program(int lang, const char* src, size_t len)
{
  ++g_gb->creates;
  g_gb->last_src.assign(src, len);
  if (lang != NEUI_SHADER_LANG_HLSL_D2D || !g_gb->create_ok) return nullptr;
  return reinterpret_cast<void*>(uintptr_t(0x5000 + g_gb->creates));
}
void NEUI_ABI gb_destroy_program(void*) { ++g_gb->destroys; }
bool NEUI_ABI gb_draw_program(neui_render_ctx_t, void*, int32_t x, int32_t y, uint32_t w, uint32_t h,
                              const float* c, uint32_t n)
{
  if (!g_gb->draw_ok) return false;
  ++g_gb->gpu_draws;
  g_gb->x = x; g_gb->y = y; g_gb->w = w; g_gb->h = h;
  g_gb->constants.assign(c, c + n);
  return true;
}
void* NEUI_ABI gb_create_bitmap(neui_render_ctx_t, uint32_t, uint32_t, const uint8_t*, float)
{
  ++g_gb->bitmaps;
  return reinterpret_cast<void*>(uintptr_t(0x9000 + g_gb->bitmaps));
}
void NEUI_ABI gb_destroy_bitmap(neui_render_ctx_t, void*) {}
void NEUI_ABI gb_draw_bitmap(neui_render_ctx_t, void*, float, float, float, float, float, float, float,
                             float, uint32_t) {}
void NEUI_ABI gb_device_transform(neui_render_ctx_t, float m[6]) { std::memcpy(m, g_gb->m, sizeof g_gb->m); }

struct NullLoader {
  static uint8_t* load(const char*, uint32_t*, uint32_t*) { return nullptr; }
  static uint8_t* load_memory(const uint8_t*, size_t, uint32_t*, uint32_t*) { return nullptr; }
  static void free_pixels(uint8_t*) {}
};
using Store = AssetStore<NullLoader>;
Store* g_store = nullptr;
int g_token = 0;
const neui_session_t k_sess = { 7 };

struct Host {
  static Store* store(neui_session_t s, uint32_t& sid)
  {
    if (s.session != 7) return nullptr;
    sid = 7;
    return g_store;
  }
  static Store* store_from_token(void* t, uint32_t& sid)
  {
    if (t != &g_token) return nullptr;
    sid = 7;
    return g_store;
  }
  static void invalidate_widget(neui_session_t, uint32_t) {}
};

struct Fixture {
  GpuBackend gb;
  Store store;
  neui_shader_api_t api = shader_api_table<Host>();
  Fixture()
  {
    g_gb = &gb;
    g_store = &store;
    auto& t = gb.table;
    t.create_shader_program = gb_create_program;
    t.destroy_shader_program = gb_destroy_program;
    t.draw_shader_program = gb_draw_program;
    t.create_bitmap = gb_create_bitmap;
    t.destroy_bitmap = gb_destroy_bitmap;
    t.draw_bitmap = gb_draw_bitmap;
    t.get_device_transform = gb_device_transform;
  }
  ~Fixture()
  {
    store.clear(&gb.table);
    g_gb = nullptr;
    g_store = nullptr;
  }
  neui_asset_t create(const char* src)
  {
    return api.create(k_sess, src, uint32_t(std::strlen(src)), "t.nsl", nullptr, 0);
  }
  bool paint(neui_asset_t sh, float x, float y, float w, float h, float res = 1.0f)
  {
    neui_painter p{};
    p.backend = &gb.table;
    p.ctx = reinterpret_cast<neui_render_ctx_t>(uintptr_t(0xC0));
    p.host_token = &g_token;
    p.resolve_shader = &ShaderApi<Host>::painter_resolve;
    neui_shader_draw_t d{};
    d.shader = sh;
    d.x = x; d.y = y; d.width = w; d.height = h;
    d.resolution = res;
    return api.draw(&p, &d);
  }
};

}  // namespace

TEST_CASE("shader gpu: on the device grid the backend's program draws, no image")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_ring_glow);
  fx.gb.m[0] = fx.gb.m[3] = 1.5f;
  fx.gb.m[4] = 30.0f;
  fx.gb.m[5] = 7.5f;
  CHECK(fx.paint(sh, 2.0f, 1.0f, 40.0f, 20.0f));
  CHECK_EQ(fx.gb.creates, 1);
  CHECK(fx.gb.last_src == nsl::emit_hlsl(*fx.store.get_slot(sh.id & 0xffff)->shader->program));
  CHECK_EQ(fx.gb.gpu_draws, 1);
  CHECK_EQ(fx.gb.bitmaps, 0);
  CHECK_EQ(fx.store.shader_cache().size(), 0u);
  // Device origin (33, 9), 60 x 30 device px.
  CHECK_EQ(fx.gb.x, 33);
  CHECK_EQ(fx.gb.y, 9);
  CHECK_EQ(fx.gb.w, 60u);
  CHECK_EQ(fx.gb.h, 30u);
  CHECK_EQ(fx.gb.constants.size(), nsl::gpu_constant_count(*fx.store.get_slot(sh.id & 0xffff)->shader->program));
  CHECK_EQ(fx.gb.constants[4], 1.5f);
  // A fractional origin: floored position, remainder in the constants.
  CHECK(fx.paint(sh, 2.25f, 1.0f, 40.0f, 20.0f));
  CHECK_EQ(fx.gb.x, 33);
  CHECK_EQ(fx.gb.w, 61u);
  CHECK_APPROX(fx.gb.constants[0], 0.375);
  // One program per shader, kept.
  CHECK(fx.paint(sh, 0.0f, 0.0f, 10.0f, 10.0f));
  CHECK_EQ(fx.gb.creates, 1);
  fx.store.release_slot(sh.id & 0xffff, &fx.gb.table);
  CHECK_EQ(fx.gb.destroys, 1);
}

TEST_CASE("shader gpu: the CPU draws everything else")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_flat);
  // Another resolution.
  CHECK(fx.paint(sh, 0, 0, 16, 16, 0.5f));
  CHECK_EQ(fx.gb.gpu_draws, 0);
  CHECK_EQ(fx.gb.bitmaps, 1);
  // Rotation.
  fx.gb.m[0] = 0.0f; fx.gb.m[1] = 1.0f; fx.gb.m[2] = -1.0f; fx.gb.m[3] = 0.0f;
  CHECK(fx.paint(sh, 0, 0, 16, 16));
  CHECK_EQ(fx.gb.gpu_draws, 0);
  CHECK_EQ(fx.gb.bitmaps, 2);
  fx.gb.m[0] = 1.0f; fx.gb.m[1] = 0.0f; fx.gb.m[2] = 0.0f; fx.gb.m[3] = 1.0f;
  // cpu_only: no program is even made.
  const neui_asset_t co = fx.create(k_cpu_only);
  CHECK(fx.paint(co, 0, 0, 16, 16));
  CHECK_EQ(fx.gb.gpu_draws, 0);
  // The session switch.
  REQUIRE(fx.api.set_gpu != nullptr);
  fx.api.set_gpu(k_sess, false);
  CHECK(fx.paint(sh, 0, 0, 16, 16));
  CHECK_EQ(fx.gb.gpu_draws, 0);
  CHECK_EQ(fx.gb.creates, 0);
  fx.api.set_gpu(k_sess, true);
  CHECK(fx.paint(sh, 0, 0, 16, 16));
  CHECK_EQ(fx.gb.gpu_draws, 1);
  CHECK_EQ(fx.gb.creates, 1);
}

TEST_CASE("shader gpu: failures fall back to the CPU")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_flat);
  // The backend cannot make the program: CPU, and no second attempt.
  fx.gb.create_ok = false;
  CHECK(fx.paint(sh, 0, 0, 16, 16));
  CHECK(fx.paint(sh, 0, 0, 16, 16));
  CHECK_EQ(fx.gb.creates, 1);
  CHECK_EQ(fx.gb.gpu_draws, 0);
  CHECK_EQ(fx.store.shader_cache().renders(), 1u);
  // A draw the backend declines: the CPU image, in the same paint.
  const neui_asset_t sh2 = fx.create(k_ring_glow);
  fx.gb.create_ok = true;
  fx.gb.draw_ok = false;
  const int bitmaps = fx.gb.bitmaps;
  CHECK(fx.paint(sh2, 0, 0, 16, 16));
  CHECK_EQ(fx.gb.bitmaps, bitmaps + 1);
  // A backend without GPU programs never sees one.
  fx.gb.table.create_shader_program = nullptr;
  const neui_asset_t sh3 = fx.create(k_everything);
  CHECK(fx.paint(sh3, 0, 0, 16, 16));
  CHECK_EQ(fx.gb.creates, 2);
}
