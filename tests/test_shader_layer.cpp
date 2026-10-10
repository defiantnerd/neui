#include "neui_test.h"

// The live shader paths (hosts/shared/shader_layer.h): the compound SHADER
// layer's props and uniform resolution, shader_paint's placement on the
// device grid, the session image cache (ShaderCache, shader_asset.h) and its
// release paths, fallback layers, and NEUI_API_SHADER's draw. A fake backend
// records uploads and draws; no host is linked.

#include "asset_store.h"
#include "painter.h"
#include "shader_api.h"
#include "shader_layer.h"
#include "widget_paint_compound.h"

#include <cmath>
#include <cstring>
#include <map>
#include <string>
#include <vector>

using namespace neui_detail;
namespace nsl = neui::nsl;

namespace {

// ---- Fake backend ----------------------------------------------------------

struct Upload {
  uint32_t w = 0, h = 0;
  float scale = 0.0f;
  std::vector<uint8_t> pixels;
};

struct FakeBackend {
  std::map<uintptr_t, Upload> live;   // handle -> upload
  uintptr_t next = 0x1000;
  int creates = 0, destroys = 0, draws = 0;
  void* last_bmp = nullptr;
  float last_dst[4] = { 0, 0, 0, 0 };
  float m[6] = { 1, 0, 0, 1, 0, 0 };  // device transform
  uint32_t generation = 0;
  neui_render_backend_t table{};
};
FakeBackend* g_fb = nullptr;
int g_asset_draws = 0;  // draw_asset_thunk calls (a fallback ASSET layer)

void NEUI_ABI count_asset_draw(void*, neui_render_backend_t*, neui_render_ctx_t, neui_asset_t,
                               float, float, float, float, uint32_t, uint32_t)
{
  ++g_asset_draws;
}

void* NEUI_ABI fb_create_bitmap(neui_render_ctx_t, uint32_t w, uint32_t h,
                                const uint8_t* px, float scale)
{
  ++g_fb->creates;
  Upload u;
  u.w = w; u.h = h; u.scale = scale;
  u.pixels.assign(px, px + size_t(w) * h * 4);
  const uintptr_t id = g_fb->next++;
  g_fb->live[id] = std::move(u);
  return reinterpret_cast<void*>(id);
}
void NEUI_ABI fb_destroy_bitmap(neui_render_ctx_t, void* bmp)
{
  ++g_fb->destroys;
  g_fb->live.erase(reinterpret_cast<uintptr_t>(bmp));
}
void NEUI_ABI fb_draw_bitmap(neui_render_ctx_t, void* bmp, float, float, float, float,
                             float dx, float dy, float dw, float dh, uint32_t)
{
  ++g_fb->draws;
  g_fb->last_bmp = bmp;
  g_fb->last_dst[0] = dx; g_fb->last_dst[1] = dy;
  g_fb->last_dst[2] = dw; g_fb->last_dst[3] = dh;
}
void NEUI_ABI fb_get_device_transform(neui_render_ctx_t, float m[6])
{
  std::memcpy(m, g_fb->m, sizeof g_fb->m);
}
uint32_t NEUI_ABI fb_generation(neui_render_ctx_t) { return g_fb->generation; }
float NEUI_ABI fb_scale(neui_render_ctx_t) { return g_fb->m[0]; }

// ---- A store, a session and a painter ---------------------------------------

struct NullLoader {
  static uint8_t* load(const char*, uint32_t*, uint32_t*) { return nullptr; }
  static uint8_t* load_memory(const uint8_t*, size_t, uint32_t*, uint32_t*) { return nullptr; }
  static void free_pixels(uint8_t*) {}
};
using Store = AssetStore<NullLoader>;

Store* g_store = nullptr;
const uint32_t k_sid = 5;
const neui_session_t k_sess = { k_sid };
int g_token = 0;  // the painters' host_token points here

struct TestHost {
  static Store* store(neui_session_t session, uint32_t& sid)
  {
    if (session.session != k_sid) return nullptr;
    sid = k_sid;
    return g_store;
  }
  static Store* store_from_token(void* token, uint32_t& sid)
  {
    if (token != &g_token) return nullptr;
    sid = k_sid;
    return g_store;
  }
  static std::vector<uint32_t> invalidated;
  static void invalidate_widget(neui_session_t session, uint32_t widget)
  {
    if (session.session == k_sid) invalidated.push_back(widget);
  }
};
std::vector<uint32_t> TestHost::invalidated;
using Api = ShaderApi<TestHost>;

neui_render_ctx_t ctx_a() { return reinterpret_cast<neui_render_ctx_t>(uintptr_t(0xA0)); }
neui_render_ctx_t ctx_b() { return reinterpret_cast<neui_render_ctx_t>(uintptr_t(0xB0)); }

struct Fixture {
  FakeBackend fb;
  Store store;
  neui_shader_api_t api = shader_api_table<TestHost>();
  Fixture()
  {
    g_fb = &fb;
    g_store = &store;
    auto& t = fb.table;
    t.create_bitmap = fb_create_bitmap;
    t.destroy_bitmap = fb_destroy_bitmap;
    t.draw_bitmap = fb_draw_bitmap;
    t.get_device_transform = fb_get_device_transform;
    t.get_context_generation = fb_generation;
    t.get_scale_factor = fb_scale;
  }
  ~Fixture()
  {
    store.clear(&fb.table);
    g_fb = nullptr;
    g_store = nullptr;
  }

  neui_painter painter(neui_render_ctx_t ctx)
  {
    neui_painter p{};
    p.backend = &fb.table;
    p.ctx = ctx;
    p.host_token = &g_token;
    p.resolve_shader = &Api::painter_resolve;
    return p;
  }
  neui_asset_t create(const char* src)
  {
    char err[256];
    neui_asset_t a = api.create(k_sess, src, uint32_t(std::strlen(src)), "t.nsl", err, sizeof err);
    if (a.id == asset_none.id) std::fprintf(stderr, "  compile error: %s\n", err);
    return a;
  }
  const ShaderAsset& shader(neui_asset_t a) { return *store.get_slot(a.id & 0xffff)->shader; }
  const Upload& last_upload() { return fb.live.at(reinterpret_cast<uintptr_t>(fb.last_bmp)); }
};

// What the executor makes of a draw, for comparing uploads against.
std::vector<uint8_t> reference(const nsl::Program& prog, const nsl::Draw& d,
                               nsl::Placement* out_pl = nullptr)
{
  const nsl::Placement pl = nsl::placement(d);
  std::vector<uint8_t> px(size_t(pl.w) * pl.h * 4);
  nsl::execute(prog, d, px.data(), size_t(pl.w) * 4);
  if (out_pl) *out_pl = pl;
  return px;
}

const char* const k_disc = R"(
uniform float value = 0.5;
uniform vec4  accent = vec4(1.0, 0.5, 0.0, 1.0);
vec4 main(vec2 p) {
  float d = length(p - neui_size * 0.5) - min(neui_size.x, neui_size.y) * 0.4;
  return vec4(accent.rgb * (0.5 + value), accent.a * clamp(0.5 - d / neui_px, 0.0, 1.0));
}
)";

const char* const k_vec = R"(
uniform float f = 1.0;
uniform vec2  v2 = vec2(2.0, 3.0);
uniform vec3  v3 = vec3(4.0, 5.0, 6.0);
uniform vec4  v4 = vec4(7.0, 8.0, 9.0, 10.0);
vec4 main(vec2 p) { return vec4(f, v2.x + v3.y, v4.z, 1.0); }
)";

const char* const k_clock = R"(
vec4 main(vec2 p) { return vec4(fract(neui_time), 0.0, 0.0, 1.0); }
)";

std::vector<float> layer_values(Fixture& fx, neui_asset_t sh, const CompoundLayer& L,
                                const AttrBag* bag)
{
  std::vector<float> out;
  shader_layer_uniforms(L, *fx.shader(sh).program, bag, out);
  return out;
}

}  // namespace

// ---- Props -------------------------------------------------------------------

TEST_CASE("shader layer: uniform prop names")
{
  std::string key;
  CHECK(shader_uniform_prop("u.value", key));
  CHECK(key == "value");
  CHECK(shader_uniform_prop("u.center.x", key));
  CHECK(key == "center.x");
  CHECK(shader_uniform_prop("u.tint.a", key));
  CHECK(shader_uniform_prop("u.step", key));  // a uniform called "step"
  CHECK(key == "step");
  CHECK_FALSE(shader_uniform_prop("u.value.step", key));  // a binding's quantisation
  CHECK_FALSE(shader_uniform_prop("u.center.q", key));
  CHECK_FALSE(shader_uniform_prop("u.a.b.c", key));
  CHECK_FALSE(shader_uniform_prop("u.", key));
  CHECK_FALSE(shader_uniform_prop("value", key));
  CHECK_EQ(shader_uniform_component("center.x"), 0);
  CHECK_EQ(shader_uniform_component("tint.g"), 1);
  CHECK_EQ(shader_uniform_component("tint.b"), 2);
  CHECK_EQ(shader_uniform_component("center.w"), 3);
  CHECK_EQ(shader_uniform_component("center"), -1);
}

TEST_CASE("shader layer: props are routed to the layer")
{
  CompoundAsset ca;
  const uint32_t s = compound_add_layer(ca, NEUI_COMPOUND_LAYER_SHADER, 0);
  CompoundLayer& L = *compound_get_layer(ca, s);
  CHECK(apply_set_asset(L, "shader", neui_asset_t{ 0x50007 }));
  CHECK_EQ(L.shader.id, 0x50007u);
  CHECK(apply_set_float(L, "resolution", 0.5f));
  CHECK(apply_set_float(L, "time", 12.5f));
  CHECK(apply_set_int(L, "fallback", int(0x00030002)));
  CHECK_EQ(L.fallback, 2u);  // a layer handle's slot
  CHECK(apply_set_float(L, "u.value", 0.25f));
  CHECK(apply_set_int(L, "u.accent", int(0x80FF0000u)));
  CHECK(apply_set_string(L, "u.center", "0.25, 0.75"));
  CHECK(apply_set_float(L, "u.center.y", 0.5f));
  CHECK(apply_set_string(L, "name", "glow"));
  CHECK(L.name == "glow");
  CHECK_FALSE(apply_set_string(L, "u.center.x", "1"));  // components take numbers
  using S = CompoundLayer::ShaderUniformStatic;
  CHECK(L.shader_uniforms.at("value").kind == S::FLOAT);
  CHECK(L.shader_uniforms.at("accent").kind == S::INT);
  CHECK(L.shader_uniforms.at("center").kind == S::VEC);
  CHECK_EQ(L.shader_uniforms.at("center").count, 2);
  // "u.value.step" is the binding's quantisation, as for every prop.
  CHECK(apply_set_float(L, "u.value.step", 0.125f));
  CHECK_EQ(L.bind_steps.at("u.value"), 0.125f);
  // Other kinds do not take shader props.
  const uint32_t r = compound_add_layer(ca, NEUI_COMPOUND_LAYER_RECT, 0);
  CHECK_FALSE(apply_set_float(*compound_get_layer(ca, r), "u.value", 1.0f));
  CHECK_FALSE(apply_set_asset(*compound_get_layer(ca, r), "shader", neui_asset_t{ 1 }));
}

TEST_CASE("shader layer: removing a fallback target clears the reference")
{
  CompoundAsset ca;
  const uint32_t s = compound_add_layer(ca, NEUI_COMPOUND_LAYER_SHADER, 0);
  const uint32_t r = compound_add_layer(ca, NEUI_COMPOUND_LAYER_RECT, 0);
  apply_set_int(*compound_get_layer(ca, s), "fallback", int(r));
  compound_remove_layer(ca, r, nullptr);
  CHECK_EQ(compound_get_layer(ca, s)->fallback, 0u);
}

// ---- Uniform resolution -------------------------------------------------------

TEST_CASE("shader layer: uniforms start at their declared defaults")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_vec);
  REQUIRE(sh.id != asset_none.id);
  CompoundLayer L;
  L.kind = NEUI_COMPOUND_LAYER_SHADER;
  const std::vector<float> v = layer_values(fx, sh, L, nullptr);
  const std::vector<float> want = { 1, 2, 3, 4, 5, 6, 7, 8, 9, 10 };
  CHECK(v == want);
}

TEST_CASE("shader layer: statics by kind")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_vec);
  CompoundLayer L;
  L.kind = NEUI_COMPOUND_LAYER_SHADER;
  apply_set_float(L, "u.f", 0.5f);                       // float
  apply_set_int(L, "u.v2", 3);                           // number on a vec2: splat
  apply_set_int(L, "u.v3", int(0x80336699u));            // colour on a vec3: rgb
  apply_set_int(L, "u.v4", int(0x80336699u));            // colour on a vec4: rgba
  apply_set_float(L, "u.unknown", 9.0f);                 // ignored
  std::vector<float> v = layer_values(fx, sh, L, nullptr);
  CHECK_EQ(v[0], 0.5f);
  CHECK_EQ(v[1], 3.0f);
  CHECK_EQ(v[2], 3.0f);
  CHECK_EQ(v[3], 51.0f / 255.0f);
  CHECK_EQ(v[4], 102.0f / 255.0f);
  CHECK_EQ(v[5], 153.0f / 255.0f);
  CHECK_EQ(v[6], 51.0f / 255.0f);
  CHECK_EQ(v[7], 102.0f / 255.0f);
  CHECK_EQ(v[8], 153.0f / 255.0f);
  CHECK_EQ(v[9], 128.0f / 255.0f);
  // A float splats, a component list fills what it names, a component
  // prop overrides one component.
  apply_set_float(L, "u.v4", 0.25f);
  apply_set_string(L, "u.v3", "1.5 2.5");
  apply_set_float(L, "u.v4.w", 0.75f);
  apply_set_int(L, "u.v2.y", 7);
  v = layer_values(fx, sh, L, nullptr);
  CHECK_EQ(v[1], 3.0f);
  CHECK_EQ(v[2], 7.0f);
  CHECK_EQ(v[3], 1.5f);
  CHECK_EQ(v[4], 2.5f);
  CHECK_EQ(v[5], 6.0f);   // replaced the colour; z not given keeps its default
  CHECK_EQ(v[6], 0.25f);
  CHECK_EQ(v[8], 0.25f);
  CHECK_EQ(v[9], 0.75f);
}

TEST_CASE("shader layer: bindings")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_vec);
  CompoundLayer L;
  L.kind = NEUI_COMPOUND_LAYER_SHADER;
  AttrBag bag;
  bag.set_float("level", 0.3f);
  bag.set_int("colour", int(0xFF102030u));
  apply_set_float(L, "u.f", 0.9f);
  apply_bind(L, "u.f", "level", 2.0f, 0.1f);             // binding wins over the static
  apply_bind(L, "u.v2", "level", 1.0f, 0.0f);            // float attr: splat
  apply_bind(L, "u.v4", "colour", 5.0f, 5.0f);           // int attr on a vec4: raw colour
  apply_bind(L, "u.v3", "level", 1.0f, 0.0f);            // float attr on a vec3: splat
  apply_set_float(L, "u.v3.y", 0.0f);                    // component static after the binding
  std::vector<float> v = layer_values(fx, sh, L, &bag);
  CHECK_EQ(v[0], 2.0f * 0.3f + 0.1f);
  CHECK_EQ(v[1], 0.3f);
  CHECK_EQ(v[2], 0.3f);
  CHECK_EQ(v[3], 0.3f);
  CHECK_EQ(v[4], 0.0f);
  CHECK_EQ(v[5], 0.3f);
  CHECK_EQ(v[6], 16.0f / 255.0f);
  CHECK_EQ(v[7], 32.0f / 255.0f);
  CHECK_EQ(v[8], 48.0f / 255.0f);
  CHECK_EQ(v[9], 1.0f);
  // Component bindings come last; the step quantises before scale / offset.
  apply_bind(L, "u.v4.x", "level", 1.0f, 0.0f);
  apply_set_float(L, "u.v4.x.step", 0.25f);
  apply_set_float(L, "u.f.step", 0.25f);
  v = layer_values(fx, sh, L, &bag);
  CHECK_EQ(v[6], 0.25f);
  CHECK_EQ(v[0], 2.0f * 0.25f + 0.1f);
}

// ---- shader_paint ---------------------------------------------------------------

TEST_CASE("shader paint: draws the executor's pixels 1:1 at scale 1")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_disc);
  neui_painter p = fx.painter(ctx_a());
  const ShaderAsset& sa = fx.shader(sh);
  CHECK(shader_paint(&p, sa, fx.store.shader_cache(), 10.0f, 20.0f, 40.0f, 30.0f, 1.0f, 0.0f,
                     nullptr));
  REQUIRE(fx.fb.draws == 1);
  nsl::Draw d;
  d.w = 40.0f; d.h = 30.0f;
  nsl::Placement pl;
  const std::vector<uint8_t> want = reference(*sa.program, d, &pl);
  const Upload& u = fx.last_upload();
  CHECK_EQ(u.w, 40u);
  CHECK_EQ(u.h, 30u);
  CHECK_EQ(u.scale, 1.0f);
  CHECK(u.pixels == want);
  CHECK_EQ(fx.fb.last_dst[0], 10.0f);
  CHECK_EQ(fx.fb.last_dst[1], 20.0f);
  CHECK_EQ(fx.fb.last_dst[2], 40.0f);
  CHECK_EQ(fx.fb.last_dst[3], 30.0f);
}

TEST_CASE("shader paint: lands on whole device px under scale and translation")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_disc);
  const ShaderAsset& sa = fx.shader(sh);
  // 150% with the widget translated to (7, 3): the layer at (1, 1) starts at
  // device (12, 6), and a quarter logical px further right at (12.375, 6).
  fx.fb.m[0] = fx.fb.m[3] = 1.5f;
  fx.fb.m[4] = 10.5f; fx.fb.m[5] = 4.5f;
  neui_painter p = fx.painter(ctx_a());
  CHECK(shader_paint(&p, sa, fx.store.shader_cache(), 1.0f, 1.0f, 20.0f, 10.0f, 1.0f, 0.0f,
                     nullptr));
  CHECK_EQ(fx.last_upload().w, 30u);
  CHECK_EQ(fx.last_upload().h, 15u);
  CHECK_EQ(fx.last_upload().scale, 1.5f);
  // Mapped back through the transform, the destination is device (12, 6).
  CHECK_APPROX(fx.fb.last_dst[0] * 1.5f + 10.5f, 12.0f);
  CHECK_APPROX(fx.fb.last_dst[1] * 1.5f + 4.5f, 6.0f);
  CHECK_APPROX(fx.fb.last_dst[2] * 1.5f, 30.0f);

  CHECK(shader_paint(&p, sa, fx.store.shader_cache(), 1.25f, 1.0f, 20.0f, 10.0f, 1.0f, 0.0f,
                     nullptr));
  const Upload& u = fx.last_upload();
  CHECK_EQ(u.w, 31u);  // the half covered edge pixels on both sides
  nsl::Draw d;
  d.x = 0.375f / 1.5f; d.w = 20.0f; d.h = 10.0f; d.scale = 1.5f;
  CHECK(u.pixels == reference(*sa.program, d));
  CHECK_APPROX(fx.fb.last_dst[0] * 1.5f + 10.5f, 12.0f);  // the floored origin
  CHECK_EQ(fx.store.shader_cache().size(), 2u);
}

TEST_CASE("shader paint: resolution scales the shading density")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_disc);
  fx.fb.m[0] = fx.fb.m[3] = 2.0f;
  neui_painter p = fx.painter(ctx_a());
  CHECK(shader_paint(&p, fx.shader(sh), fx.store.shader_cache(), 0, 0, 40, 20, 0.5f, 0, nullptr));
  CHECK_EQ(fx.last_upload().w, 40u);  // 2x display at half density
  CHECK_EQ(fx.fb.last_dst[2], 40.0f);  // still covers the layer
  CHECK(shader_paint(&p, fx.shader(sh), fx.store.shader_cache(), 0, 0, 40, 20, 0.0f, 0, nullptr));
  CHECK_EQ(fx.last_upload().w, 80u);  // <= 0 means 1
  CHECK(shader_paint(&p, fx.shader(sh), fx.store.shader_cache(), 0, 0, 40, 20, 100.f, 0, nullptr));
  CHECK_EQ(fx.last_upload().w, 320u);  // clamped to 4
}

TEST_CASE("shader paint: a rotated painter shades in layer space")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_disc);
  const float c = std::cos(0.5f) * 2.0f, s = std::sin(0.5f) * 2.0f;
  fx.fb.m[0] = c; fx.fb.m[1] = s; fx.fb.m[2] = -s; fx.fb.m[3] = c;
  fx.fb.m[4] = 33.3f; fx.fb.m[5] = 17.7f;
  neui_painter p = fx.painter(ctx_a());
  CHECK(shader_paint(&p, fx.shader(sh), fx.store.shader_cache(), 5, 5, 30, 20, 1.0f, 0, nullptr));
  CHECK_EQ(fx.last_upload().w, 60u);  // at the transform's scale, 2
  CHECK_APPROX(fx.fb.last_dst[0], 5.0f);
  CHECK_APPROX(fx.fb.last_dst[1], 5.0f);
  CHECK_APPROX(fx.fb.last_dst[2], 30.0f);
  CHECK_APPROX(fx.fb.last_dst[3], 20.0f);
}

TEST_CASE("shader paint: empty or oversized layers draw nothing")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_disc);
  neui_painter p = fx.painter(ctx_a());
  ShaderCache& cache = fx.store.shader_cache();
  CHECK_FALSE(shader_paint(&p, fx.shader(sh), cache, 0, 0, 0, 10, 1, 0, nullptr));
  CHECK_FALSE(shader_paint(&p, fx.shader(sh), cache, 0, 0, 10, -1, 1, 0, nullptr));
  CHECK_FALSE(shader_paint(&p, fx.shader(sh), cache, 0, 0, 20000, 10, 1, 0, nullptr));
  fx.fb.m[0] = fx.fb.m[3] = 0.0f;
  CHECK_FALSE(shader_paint(&p, fx.shader(sh), cache, 0, 0, 10, 10, 1, 0, nullptr));
  CHECK_EQ(fx.fb.draws, 0);
  CHECK_EQ(cache.size(), 0u);
}

// ---- The image cache -------------------------------------------------------------

TEST_CASE("shader cache: same inputs share one image; each ctx uploads once")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_disc);
  ShaderCache& cache = fx.store.shader_cache();
  neui_painter pa = fx.painter(ctx_a()), pb = fx.painter(ctx_b());
  std::vector<float> v;
  shader_uniform_values(*fx.shader(sh).program, nullptr, 0, v);
  for (int i = 0; i < 3; ++i) {
    CHECK(shader_paint(&pa, fx.shader(sh), cache, 4, 4, 32, 32, 1, 0, v.data()));
    CHECK(shader_paint(&pb, fx.shader(sh), cache, 50, 8, 32, 32, 1, 0, v.data()));
  }
  CHECK_EQ(cache.renders(), 1u);  // both at whole px: one image
  CHECK_EQ(cache.size(), 1u);
  CHECK_EQ(fx.fb.creates, 2);     // one upload per ctx
  CHECK_EQ(fx.fb.draws, 6);
  // Different uniforms: a new image.
  v[0] = 0.75f;
  CHECK(shader_paint(&pa, fx.shader(sh), cache, 4, 4, 32, 32, 1, 0, v.data()));
  CHECK_EQ(cache.renders(), 2u);
  // Time does not split the cache for a program that does not read it.
  CHECK(shader_paint(&pa, fx.shader(sh), cache, 4, 4, 32, 32, 1, 12.0f, v.data()));
  CHECK_EQ(cache.renders(), 2u);
  // It does for one that does.
  const neui_asset_t clock = fx.create(k_clock);
  CHECK(shader_paint(&pa, fx.shader(clock), cache, 0, 0, 8, 8, 1, 1.0f, nullptr));
  CHECK(shader_paint(&pa, fx.shader(clock), cache, 0, 0, 8, 8, 1, 1.0f, nullptr));
  CHECK(shader_paint(&pa, fx.shader(clock), cache, 0, 0, 8, 8, 1, 1.5f, nullptr));
  CHECK_EQ(cache.renders(), 4u);
}

TEST_CASE("shader cache: least recently used images go past the budget")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_disc);
  ShaderCache& cache = fx.store.shader_cache();
  neui_painter p = fx.painter(ctx_a());
  const size_t one = 16 * 16 * 4;
  cache.set_budget(3 * one, &fx.fb.table);
  std::vector<float> v;
  shader_uniform_values(*fx.shader(sh).program, nullptr, 0, v);
  for (int i = 0; i < 3; ++i) {
    v[0] = float(i);
    shader_paint(&p, fx.shader(sh), cache, 0, 0, 16, 16, 1, 0, v.data());
  }
  CHECK_EQ(cache.size(), 3u);
  // Touch image 0, then add a fourth: image 1 is the least recently used.
  v[0] = 0.0f;
  shader_paint(&p, fx.shader(sh), cache, 0, 0, 16, 16, 1, 0, v.data());
  v[0] = 3.0f;
  shader_paint(&p, fx.shader(sh), cache, 0, 0, 16, 16, 1, 0, v.data());
  CHECK_EQ(cache.size(), 3u);
  CHECK_EQ(cache.bytes(), 3 * one);
  CHECK_EQ(fx.fb.destroys, 1);  // its upload went with it
  CHECK_EQ(fx.fb.live.size(), 3u);
  const uint64_t renders = cache.renders();
  v[0] = 0.0f;
  shader_paint(&p, fx.shader(sh), cache, 0, 0, 16, 16, 1, 0, v.data());
  CHECK_EQ(cache.renders(), renders);      // still there
  v[0] = 1.0f;
  shader_paint(&p, fx.shader(sh), cache, 0, 0, 16, 16, 1, 0, v.data());
  CHECK_EQ(cache.renders(), renders + 1);  // re-rendered
  // An image larger than the whole budget is still drawn, alone.
  shader_paint(&p, fx.shader(sh), cache, 0, 0, 64, 64, 1, 0, v.data());
  CHECK_EQ(cache.size(), 1u);
  CHECK_EQ(fx.fb.live.size(), 1u);
}

TEST_CASE("shader cache: a generation bump re-uploads")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_disc);
  neui_painter p = fx.painter(ctx_a());
  shader_paint(&p, fx.shader(sh), fx.store.shader_cache(), 0, 0, 16, 16, 1, 0, nullptr);
  fx.fb.generation = 1;
  shader_paint(&p, fx.shader(sh), fx.store.shader_cache(), 0, 0, 16, 16, 1, 0, nullptr);
  CHECK_EQ(fx.fb.creates, 2);
  CHECK_EQ(fx.fb.destroys, 1);
  CHECK_EQ(fx.store.shader_cache().renders(), 1u);
}

TEST_CASE("shader cache: released with the context, the shader and the store")
{
  Fixture fx;
  const neui_asset_t a = fx.create(k_disc);
  const neui_asset_t b = fx.create(k_clock);
  ShaderCache& cache = fx.store.shader_cache();
  neui_painter pa = fx.painter(ctx_a()), pb = fx.painter(ctx_b());
  neui_painter* painters[] = { &pa, &pb };
  for (neui_painter* p : painters) {
    shader_paint(p, fx.shader(a), cache, 0, 0, 16, 16, 1, 0, nullptr);
    shader_paint(p, fx.shader(b), cache, 0, 0, 16, 16, 1, 0, nullptr);
  }
  CHECK_EQ(fx.fb.live.size(), 4u);
  fx.store.release_context(ctx_b(), &fx.fb.table);  // uploads go, images stay
  CHECK_EQ(fx.fb.live.size(), 2u);
  CHECK_EQ(cache.size(), 2u);
  fx.store.release_slot(a.id & 0xffff, &fx.fb.table);  // the shader's images go
  CHECK_EQ(fx.fb.live.size(), 1u);
  CHECK_EQ(cache.size(), 1u);
  fx.store.clear(&fx.fb.table);
  CHECK_EQ(fx.fb.live.size(), 0u);
  CHECK_EQ(cache.size(), 0u);
  CHECK_EQ(cache.bytes(), 0u);
}

// ---- The compound layer ------------------------------------------------------------

TEST_CASE("shader layer: paints through the compound, fallback hidden")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_disc);
  CompoundAsset ca;
  const uint32_t fb_slot = compound_add_layer(ca, NEUI_COMPOUND_LAYER_ASSET, 0);
  apply_set_asset(*compound_get_layer(ca, fb_slot), "asset", neui_asset_t{ 0x77 });
  const uint32_t s = compound_add_layer(ca, NEUI_COMPOUND_LAYER_SHADER, 1);
  CompoundLayer& L = *compound_get_layer(ca, s);
  L.width = 20; L.height = 20;
  L.parent_anchor = L.self_anchor = NEUI_ANCHOR_CENTER;
  apply_set_asset(L, "shader", sh);
  apply_set_int(L, "fallback", int(fb_slot));
  AttrBag bag;
  bag.set_float("v", 0.25f);
  apply_bind(L, "u.value", "v", 1.0f, 0.0f);

  neui_painter p = fx.painter(ctx_a());
  p.draw_asset_thunk = count_asset_draw;
  g_asset_draws = 0;
  paint_compound_above(&p, ca, 40.0f, 40.0f, &bag, NEUI_LAYER_STATE_ENABLED);
  CHECK_EQ(g_asset_draws, 0);  // hidden: the shader renders
  CHECK_EQ(fx.fb.draws, 1);
  CHECK_EQ(fx.fb.last_dst[0], 10.0f);
  CHECK_EQ(fx.fb.last_dst[2], 20.0f);
  nsl::Draw d;
  d.w = 20.0f; d.h = 20.0f;
  std::vector<float> v = { 0.25f, 1.0f, 0.5f, 0.0f, 1.0f };
  d.uniforms = v.data();
  CHECK(fx.last_upload().pixels == reference(*fx.shader(sh).program, d));

  // Without shader support the shader layer paints nothing, the fallback does.
  p.resolve_shader = nullptr;
  g_asset_draws = 0;
  paint_compound_above(&p, ca, 40.0f, 40.0f, &bag, NEUI_LAYER_STATE_ENABLED);
  CHECK_EQ(g_asset_draws, 1);
  CHECK_EQ(fx.fb.draws, 1);
  // Likewise with no shader asset.
  p.resolve_shader = &Api::painter_resolve;
  apply_set_asset(L, "shader", asset_none);
  g_asset_draws = 0;
  paint_compound_above(&p, ca, 40.0f, 40.0f, &bag, NEUI_LAYER_STATE_ENABLED);
  CHECK_EQ(g_asset_draws, 1);
  // And with a stale one.
  apply_set_asset(L, "shader", sh);
  fx.store.release_slot(sh.id & 0xffff, &fx.fb.table);
  g_asset_draws = 0;
  paint_compound_above(&p, ca, 40.0f, 40.0f, &bag, NEUI_LAYER_STATE_ENABLED);
  CHECK_EQ(g_asset_draws, 1);
  CHECK_EQ(fx.fb.draws, 1);
}

TEST_CASE("shader layer: the resolve thunk rejects foreign handles")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_disc);
  const ShaderAsset* sa = nullptr;
  ShaderCache* cache = nullptr;
  CHECK(Api::painter_resolve(&g_token, sh, &sa, &cache));
  CHECK(sa == &fx.shader(sh));
  CHECK(cache == &fx.store.shader_cache());
  int other = 0;
  CHECK_FALSE(Api::painter_resolve(&other, sh, &sa, &cache));  // another session's painter
  CHECK_FALSE(Api::painter_resolve(&g_token, neui_asset_t{ (6u << 16) | (sh.id & 0xffff) },
                                   &sa, &cache));
  CHECK_FALSE(Api::painter_resolve(&g_token, asset_none, &sa, &cache));
  const uint32_t comp = fx.store.allocate_compound();
  CHECK_FALSE(Api::painter_resolve(&g_token, neui_asset_t{ (k_sid << 16) | comp }, &sa, &cache));
}

// ---- NEUI_API_SHADER draw -----------------------------------------------------------

TEST_CASE("shader api: draw renders through the cache with named uniforms")
{
  Fixture fx;
  const neui_asset_t sh = fx.create(k_disc);
  REQUIRE(fx.api.draw != nullptr);
  neui_painter p = fx.painter(ctx_a());
  const neui_shader_uniform_t u[] = { { "value", { 1.0f, 0, 0, 0 } },
                                      neui_shader_color("accent", 0xFF00FF00u),
                                      { "nope", { 5, 5, 5, 5 } } };
  neui_shader_draw_t d{};
  d.shader = sh;
  d.x = 2; d.y = 3; d.width = 16; d.height = 16;
  d.uniforms = u;
  d.uniform_count = 3;
  CHECK(fx.api.draw(&p, &d));
  CHECK_EQ(fx.fb.last_dst[0], 2.0f);
  CHECK_EQ(fx.fb.last_dst[1], 3.0f);
  nsl::Draw nd;
  nd.w = 16; nd.h = 16;
  const std::vector<float> v = { 1.0f, 0.0f, 1.0f, 0.0f, 1.0f };
  nd.uniforms = v.data();
  CHECK(fx.last_upload().pixels == reference(*fx.shader(sh).program, nd));
  CHECK(fx.api.draw(&p, &d));
  CHECK_EQ(fx.store.shader_cache().renders(), 1u);
  // Bad input draws nothing.
  CHECK_FALSE(fx.api.draw(nullptr, &d));
  CHECK_FALSE(fx.api.draw(&p, nullptr));
  d.shader = asset_none;
  CHECK_FALSE(fx.api.draw(&p, &d));
  d.shader = sh;
  p.resolve_shader = nullptr;
  CHECK_FALSE(fx.api.draw(&p, &d));
  CHECK_EQ(fx.fb.draws, 2);
}

// ---- The animation clock -------------------------------------------------------------

namespace {
uint64_t g_now = 0;
uint64_t fake_now() { return g_now; }
const uint64_t k_ms = 1000000ull;

// A 10 x 10 SHADER layer over the whole widget.
CompoundLayer& clock_layer(CompoundAsset& ca, neui_asset_t sh)
{
  const uint32_t s = compound_add_layer(ca, NEUI_COMPOUND_LAYER_SHADER, 0);
  CompoundLayer& L = *compound_get_layer(ca, s);
  L.width = 10; L.height = 10;
  apply_set_asset(L, "shader", sh);
  return L;
}

// Paints the compound as a host paints a widget; true when it read the clock.
bool paint_widget(Fixture& fx, neui_painter& p, const CompoundAsset& ca, const AttrBag* bag,
                  uint32_t widget)
{
  AnimClock& c = fx.store.shader_cache().clock();
  c.begin_widget();
  paint_compound_above(&p, ca, 10.0f, 10.0f, bag, NEUI_LAYER_STATE_ENABLED);
  if (!c.end_widget()) return false;
  c.mark(widget, 1);
  return true;
}
}  // namespace

TEST_CASE("shader layer: a time neither set nor bound runs on the animation clock")
{
  Fixture fx;
  g_now = 0;
  fx.store.shader_cache().clock() = AnimClock(&fake_now);
  const neui_asset_t sh = fx.create(k_clock);
  CompoundAsset ca;
  CompoundLayer& L = clock_layer(ca, sh);
  AttrBag bag;
  neui_painter p = fx.painter(ctx_a());

  g_now = 250 * k_ms;
  CHECK(paint_widget(fx, p, ca, &bag, 42));
  CHECK_EQ(fx.last_upload().pixels[2], uint8_t(64));  // fract(0.25) * 255, rounded
  AnimClock& c = fx.store.shader_cache().clock();
  CHECK(c.has_owner(1));

  // A frame later the image follows the clock.
  g_now = 300 * k_ms;
  std::vector<uint32_t> inv;
  c.tick([&](uint32_t w, uint32_t) { inv.push_back(w); });
  REQUIRE(inv.size() == 1u);
  CHECK_EQ(inv[0], 42u);
  CHECK(paint_widget(fx, p, ca, &bag, 42));
  CHECK_EQ(fx.last_upload().pixels[2], uint8_t(77));  // fract(0.3) * 255

  // A bound time is the client's: no clock.
  bag.set_float("t", 0.5f);
  apply_bind(L, "time", "t", 1.0f, 0.0f);
  CHECK_FALSE(paint_widget(fx, p, ca, &bag, 42));
  CHECK_EQ(fx.last_upload().pixels[2], uint8_t(128));
  // So is a set one, even 0.
  CompoundAsset ca2;
  CompoundLayer& L2 = clock_layer(ca2, sh);
  apply_set_float(L2, "time", 0.0f);
  CHECK_FALSE(paint_widget(fx, p, ca2, &bag, 43));
  // A shader that does not read neui_time never touches the clock.
  CompoundAsset ca3;
  clock_layer(ca3, fx.create(k_disc));
  CHECK_FALSE(paint_widget(fx, p, ca3, &bag, 44));
}

TEST_CASE("shader cache: a clock image goes when the next frame's is made")
{
  Fixture fx;
  g_now = 0;
  fx.store.shader_cache().clock() = AnimClock(&fake_now);
  ShaderCache& cache = fx.store.shader_cache();
  const neui_asset_t sh = fx.create(k_clock);
  const neui_asset_t still = fx.create(k_disc);
  CompoundAsset ca, cb;
  clock_layer(ca, sh);
  clock_layer(cb, still);
  AttrBag bag;
  neui_painter p = fx.painter(ctx_a());
  paint_widget(fx, p, cb, &bag, 2);
  for (int frame = 1; frame <= 5; ++frame) {
    g_now = uint64_t(frame) * 40 * k_ms;
    cache.clock().tick([](uint32_t, uint32_t) {});
    CHECK(paint_widget(fx, p, ca, &bag, 1));
    CHECK(paint_widget(fx, p, ca, &bag, 3));  // a second widget shares the frame's image
  }
  CHECK_EQ(cache.size(), 2u);    // this frame's clock image and the still one
  CHECK_EQ(cache.renders(), 6u);
  CHECK_EQ(fx.fb.live.size(), 2u);  // the dropped images' uploads went with them
}

TEST_CASE("shader api: animation_time, pause and resume")
{
  Fixture fx;
  g_now = 0;
  fx.store.shader_cache().clock() = AnimClock(&fake_now);
  AnimClock& c = fx.store.shader_cache().clock();
  REQUIRE(fx.api.animation_time != nullptr);
  REQUIRE(fx.api.set_animation_fps != nullptr);
  REQUIRE(fx.api.set_animation_paused != nullptr);

  // Read in a paint, it makes the widget animate; draw() at that time keeps
  // one image per frame.
  const neui_asset_t sh = fx.create(k_clock);
  neui_painter p = fx.painter(ctx_a());
  neui_shader_draw_t d{};
  d.shader = sh;
  d.width = 8; d.height = 8;
  for (int frame = 0; frame < 3; ++frame) {
    g_now = uint64_t(frame) * 40 * k_ms + 100 * k_ms;
    c.tick([](uint32_t, uint32_t) {});
    c.begin_widget();
    d.time = fx.api.animation_time(k_sess);
    CHECK(fx.api.draw(&p, &d));
    REQUIRE(c.end_widget());
    c.mark(9, 0);
  }
  CHECK_APPROX(d.time, 0.18f);
  CHECK_EQ(fx.store.shader_cache().size(), 1u);
  CHECK_EQ(fx.api.animation_time(neui_session_t{ 99 }), 0.0f);

  fx.api.set_animation_fps(k_sess, 60);
  CHECK_EQ(c.fps(), 60u);

  TestHost::invalidated.clear();
  fx.api.set_animation_paused(k_sess, true);
  CHECK(c.paused());
  CHECK_EQ(c.heartbeat_ms(0), 0u);
  fx.api.set_animation_paused(k_sess, true);
  CHECK(TestHost::invalidated.empty());
  fx.api.set_animation_paused(k_sess, false);  // resuming repaints the animating widgets
  REQUIRE(TestHost::invalidated.size() == 1u);
  CHECK_EQ(TestHost::invalidated[0], 9u);
  fx.api.set_animation_paused(k_sess, false);
  CHECK_EQ(TestHost::invalidated.size(), 1u);
}

// ---- The component hook ---------------------------------------------------------------

namespace {
std::string g_seen_dir;
bool NEUI_ABI provide_glow(void*, const neui_resource_request_t* req, neui_resource_bytes_t* out)
{
  if (std::strcmp(req->name, "glow.nsl") != 0 || req->kind != NEUI_RESOURCE_KIND_SHADER)
    return false;
  g_seen_dir = req->base_dir ? req->base_dir : "";
  out->data = reinterpret_cast<const uint8_t*>(k_disc);
  out->len = uint32_t(std::strlen(k_disc));
  return true;
}
}  // namespace

TEST_CASE("shader api: component shaders compile inline source or a file")
{
  Fixture fx;
  const neui_asset_t inl = Api::component_shader(k_sess, "inline", k_clock,
                                                 uint32_t(std::strlen(k_clock)), nullptr);
  CHECK(inl.id != asset_none.id);
  CHECK(fx.store.get_slot(inl.id & 0xffff)->kind == NEUI_ASSET_KIND_SHADER);
  // Files come from the client resource provider first, with the base dir.
  neui_resource_client_t rc{};
  rc.provide = provide_glow;
  ResourceProvider prov;
  prov.client = &rc;
  fx.store.set_resource_provider(prov);
  const neui_asset_t f = Api::component_shader(k_sess, "glow.nsl", nullptr, 0, "skins/a");
  CHECK(f.id != asset_none.id);
  CHECK(g_seen_dir == "skins/a");
  CHECK_EQ(Api::component_shader(k_sess, "missing.nsl", nullptr, 0, "skins/a").id, asset_none.id);
  const char* bad = "vec4 main(vec2 p) { return q; }";
  CHECK_EQ(Api::component_shader(k_sess, "bad", bad, uint32_t(std::strlen(bad)), nullptr).id,
           asset_none.id);
  CHECK_EQ(Api::component_shader(neui_session_t{ 9 }, "x", k_clock, 10, nullptr).id,
           asset_none.id);
}
