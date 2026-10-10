#include "neui_test.h"

// NEUI_API_SHADER (include/neui/d/shader.h) through the shared implementation
// every host uses (hosts/shared/shader_api.h + shader_asset.h), over a real
// AssetStore and a fake host policy: compile + diagnostics, uniform
// introspection, bake_bitmap / bake_filmstrip against the executor, the
// strip grid, the resource-provider path, and a cross-platform golden.

#include "asset_store.h"
#include "painter.h"
#include "shader_api.h"

#include <algorithm>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace neui_detail;
namespace nsl = neui::nsl;

namespace {

struct NullLoader {
  static uint8_t* load(const char*, uint32_t*, uint32_t*) { return nullptr; }
  static uint8_t* load_memory(const uint8_t*, size_t, uint32_t*, uint32_t*) { return nullptr; }
  static void free_pixels(uint8_t*) {}
};

using Store = AssetStore<NullLoader>;

Store* g_store = nullptr;
const uint32_t k_sid = 3;
const neui_session_t k_sess = { k_sid };

struct TestShaderHost {
  static Store* store(neui_session_t session, uint32_t& session_id)
  {
    if (session.session != k_sid) return nullptr;
    session_id = k_sid;
    return g_store;
  }
};

// One fresh store per test case.
struct Fixture {
  Store store;
  neui_shader_api_t api = shader_api_table<TestShaderHost>();
  Fixture() { g_store = &store; }
  ~Fixture() { g_store = nullptr; }

  neui_asset_t create(const char* src, const char* name = "test.nsl")
  {
    char err[256];
    neui_asset_t a = api.create(k_sess, src, static_cast<uint32_t>(std::strlen(src)), name,
                                err, sizeof err);
    if (a.id == asset_none.id) std::fprintf(stderr, "  compile error: %s\n", err);
    return a;
  }
  AssetEntry* entry(neui_asset_t a) { return store.get_slot(a.id & 0xffff); }
};

uint32_t crc32(const void* data, size_t len)
{
  uint32_t crc = ~0u;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) {
    crc ^= p[i];
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

const char* const k_clock = R"(
vec4 main(vec2 p) {
  return vec4(fract(neui_time * 0.37 + p.x * 0.01), fract(neui_time), 0.5, 1.0);
})";

// The bytes execute() gives for a draw at the origin.
std::vector<uint8_t> run(const nsl::Program& prog, float w, float h, float scale, float time,
                         const std::vector<float>& uniforms)
{
  nsl::Draw d;
  d.w = w;
  d.h = h;
  d.scale = scale;
  d.time = time;
  d.uniforms = uniforms.empty() ? nullptr : uniforms.data();
  const nsl::Placement pl = nsl::placement(d);
  std::vector<uint8_t> px(static_cast<size_t>(pl.w) * pl.h * 4u);
  nsl::execute(prog, d, px.data(), static_cast<size_t>(pl.w) * 4u);
  return px;
}

// Cell (col, row) of a strip, tightly packed.
std::vector<uint8_t> cell(const AssetEntry& e, uint32_t cw, uint32_t ch, uint32_t col, uint32_t row)
{
  std::vector<uint8_t> out;
  for (uint32_t y = 0; y < ch; ++y) {
    const uint8_t* src = e.pixels.data() + ((size_t)(row * ch + y) * e.width_px + col * cw) * 4u;
    out.insert(out.end(), src, src + cw * 4u);
  }
  return out;
}

}  // namespace

TEST_CASE("shader api: create compiles into a SHADER asset")
{
  Fixture f;
  char err[128] = "stale";
  neui_asset_t a = f.api.create(k_sess, k_ring_glow, static_cast<uint32_t>(std::strlen(k_ring_glow)),
                                "ring.nsl", err, sizeof err);
  REQUIRE(a.id != asset_none.id);
  CHECK_EQ(a.id >> 16, k_sid);
  REQUIRE(f.entry(a) != nullptr);
  CHECK_EQ(f.entry(a)->kind, NEUI_ASSET_KIND_SHADER);
  CHECK(f.entry(a)->shader != nullptr);
  CHECK_EQ(std::string(err), std::string());  // cleared on success

  // Each asset gets its own never-reused uid, even when its slot is reused.
  const uint64_t uid = f.entry(a)->shader->uid;
  f.store.release_slot(a.id & 0xffff, nullptr);
  neui_asset_t b = f.create(k_ring_glow);
  CHECK_EQ(b.id, a.id);
  CHECK(f.entry(b)->shader->uid != uid);

  // The length bounds the source; it need not be NUL-terminated.
  const std::string padded = std::string("vec4 main(vec2 p) { return vec4(1.0); }") + "garbage";
  CHECK(f.api.create(k_sess, padded.data(), static_cast<uint32_t>(padded.size() - 7), nullptr,
                     nullptr, 0).id != asset_none.id);
}

TEST_CASE("shader api: a compile error returns asset_none and the diagnostic")
{
  Fixture f;
  const char* bad = "vec4 main(vec2 p) {\n  return vec4(radius);\n}";
  const uint32_t len = static_cast<uint32_t>(std::strlen(bad));
  char err[256];
  CHECK_EQ(f.api.create(k_sess, bad, len, "bad.nsl", err, sizeof err).id, asset_none.id);
  const std::string full = err;
  CHECK(full.rfind("bad.nsl:2:15: error: ", 0) == 0);
  CHECK(full.find("radius") != std::string::npos);

  CHECK_EQ(f.api.create(k_sess, bad, len, nullptr, err, sizeof err).id, asset_none.id);
  CHECK(std::string(err).rfind("<source>:2:15: error: ", 0) == 0);

  // Truncated to the buffer, always terminated; a null buffer is fine.
  char small[8];
  std::memset(small, 'x', sizeof small);
  CHECK_EQ(f.api.create(k_sess, bad, len, "bad.nsl", small, sizeof small).id, asset_none.id);
  CHECK_EQ(std::string(small), full.substr(0, 7));
  CHECK_EQ(f.api.create(k_sess, bad, len, "bad.nsl", nullptr, 0).id, asset_none.id);
  CHECK_EQ(f.api.create(k_sess, nullptr, 0, nullptr, err, sizeof err).id, asset_none.id);

  // A session the host does not know.
  CHECK_EQ(f.api.create(neui_session_t{ 9 }, k_ring_glow, 10, nullptr, err, sizeof err).id,
           asset_none.id);
  CHECK_EQ(std::string(err), std::string("invalid session"));
}

TEST_CASE("shader api: uniform introspection lists the declarations")
{
  Fixture f;
  neui_asset_t a = f.create(k_ring_glow);
  CHECK_EQ(f.api.uniform_count(k_sess, a), 2u);

  neui_shader_uniform_info_t u{};
  REQUIRE(f.api.uniform_at(k_sess, a, 0, &u));
  CHECK_EQ(std::string(u.name), std::string("value"));
  CHECK_EQ(u.components, 1u);
  CHECK_EQ(u.def[0], 0.65f);
  CHECK_EQ(u.def[1], 0.0f);
  REQUIRE(f.api.uniform_at(k_sess, a, 1, &u));
  CHECK_EQ(std::string(u.name), std::string("accent"));
  CHECK_EQ(u.components, 4u);
  CHECK_EQ(u.def[0], 0.2f);
  CHECK_EQ(u.def[3], 1.0f);
  CHECK_FALSE(f.api.uniform_at(k_sess, a, 2, &u));
  CHECK_FALSE(f.api.uniform_at(k_sess, a, 0, nullptr));

  // Not a shader, another session's handle, a released handle.
  const uint8_t px[4] = { 0, 0, 0, 0 };
  neui_asset_t bmp = { (k_sid << 16) | f.store.allocate_bitmap(1, 1, px, 1.0f) };
  CHECK_EQ(f.api.uniform_count(k_sess, bmp), 0u);
  CHECK_EQ(f.api.uniform_count(k_sess, neui_asset_t{ (4u << 16) | (a.id & 0xffff) }), 0u);
  CHECK_EQ(f.api.uniform_count(k_sess, asset_none), 0u);
  f.store.release_slot(a.id & 0xffff, nullptr);
  CHECK_EQ(f.api.uniform_count(k_sess, a), 0u);
}

TEST_CASE("shader api: bake_bitmap renders what the executor renders")
{
  Fixture f;
  neui_asset_t a = f.create(k_ring_glow);
  const neui_shader_uniform_t values[] = {
    { "value", { 0.3f, 0, 0, 0 } },
    neui_shader_color("accent", 0x80FF4020u),
    { "no_such_uniform", { 9, 9, 9, 9 } },  // ignored
    { nullptr, { 0, 0, 0, 0 } },            // ignored
  };
  neui_shader_bake_t bake{};
  bake.width = 30.0f;
  bake.height = 20.0f;
  bake.scale = 1.5f;
  bake.uniforms = values;
  bake.uniform_count = 4;
  neui_asset_t b = f.api.bake_bitmap(k_sess, a, &bake);
  REQUIRE(b.id != asset_none.id);
  AssetEntry* e = f.entry(b);
  REQUIRE(e != nullptr);
  CHECK_EQ(e->kind, NEUI_ASSET_KIND_BITMAP);
  CHECK_EQ(e->width_px, 45u);
  CHECK_EQ(e->height_px, 30u);
  CHECK_EQ(e->scale, 1.5f);
  CHECK(e->filmstrip == nullptr);

  // The colour helper is the language's exact byte / 255 rule, straight RGBA.
  CHECK_EQ(values[1].v[0], 1.0f);
  CHECK_EQ(values[1].v[1], 64.0f / 255.0f);
  CHECK_EQ(values[1].v[2], 32.0f / 255.0f);
  CHECK_EQ(values[1].v[3], 128.0f / 255.0f);

  const nsl::Program& prog = *f.entry(a)->shader->program;
  std::vector<float> u = { 0.3f, values[1].v[0], values[1].v[1], values[1].v[2], values[1].v[3] };
  CHECK(e->pixels == run(prog, 30.0f, 20.0f, 1.5f, 0.0f, u));

  // No uniforms: the declared defaults.
  bake.uniforms = nullptr;
  bake.uniform_count = 0;
  bake.scale = 0.0f;  // means 1
  neui_asset_t c = f.api.bake_bitmap(k_sess, a, &bake);
  REQUIRE(c.id != asset_none.id);
  CHECK_EQ(f.entry(c)->scale, 1.0f);
  CHECK(f.entry(c)->pixels == run(prog, 30.0f, 20.0f, 1.0f, 0.0f, { 0.65f, 0.2f, 0.7f, 1.0f, 1.0f }));

  // A fractional size covers the partial pixel.
  bake.width = 10.25f;
  neui_asset_t d = f.api.bake_bitmap(k_sess, a, &bake);
  REQUIRE(d.id != asset_none.id);
  CHECK_EQ(f.entry(d)->width_px, 11u);
}

TEST_CASE("shader api: bake time wraps to [0, 3600)")
{
  Fixture f;
  neui_asset_t a = f.create(k_clock);
  neui_shader_bake_t bake{};
  bake.width = 16.0f;
  bake.height = 4.0f;
  auto at = [&](float t) {
    bake.time = t;
    neui_asset_t b = f.api.bake_bitmap(k_sess, a, &bake);
    return b.id == asset_none.id ? std::vector<uint8_t>() : f.entry(b)->pixels;
  };
  const std::vector<uint8_t> t125 = at(1.25f);
  REQUIRE(!t125.empty());
  CHECK(at(3601.25f) == t125);
  CHECK(at(1.25f - 3600.0f * 4.0f) == t125);
  CHECK(at(-1.0f) == at(3599.0f));
  CHECK(at(0.0f) != t125);

  CHECK_EQ(nsl::wrap_time(3600.0), 0.0f);
  CHECK_EQ(nsl::wrap_time(-0.0), 0.0f);
  CHECK_EQ(nsl::wrap_time(3599.99999999), 0.0f);  // rounds up to 3600.0f
  CHECK_EQ(nsl::wrap_time(7200.5), 0.5f);
  CHECK_EQ(nsl::wrap_time(std::numeric_limits<double>::quiet_NaN()), 0.0f);
  CHECK_EQ(nsl::wrap_time(std::numeric_limits<double>::infinity()), 0.0f);
}

TEST_CASE("shader api: bake rejects bad input")
{
  Fixture f;
  neui_asset_t a = f.create(k_ring_glow);
  neui_shader_bake_t bake{};
  bake.width = 10.0f;
  bake.height = 10.0f;
  CHECK(f.api.bake_bitmap(k_sess, a, &bake).id != asset_none.id);

  auto fails = [&](neui_shader_bake_t b) { return f.api.bake_bitmap(k_sess, a, &b).id == asset_none.id; };
  neui_shader_bake_t b = bake;
  b.width = 0.0f;
  CHECK(fails(b));
  b = bake;
  b.height = -3.0f;
  CHECK(fails(b));
  b = bake;
  b.width = std::numeric_limits<float>::quiet_NaN();
  CHECK(fails(b));
  b = bake;
  b.height = std::numeric_limits<float>::infinity();
  CHECK(fails(b));
  b = bake;
  b.scale = std::numeric_limits<float>::infinity();
  CHECK(fails(b));
  b = bake;
  b.width = 8193.0f;
  b.scale = 2.0f;  // 16386 px, over the side limit
  CHECK(fails(b));

  CHECK_EQ(f.api.bake_bitmap(k_sess, a, nullptr).id, asset_none.id);
  CHECK_EQ(f.api.bake_bitmap(k_sess, asset_none, &bake).id, asset_none.id);
  CHECK_EQ(f.api.bake_bitmap(neui_session_t{ 9 }, a, &bake).id, asset_none.id);
  const uint8_t px[4] = { 0, 0, 0, 0 };
  neui_asset_t bmp = { (k_sid << 16) | f.store.allocate_bitmap(1, 1, px, 1.0f) };
  CHECK_EQ(f.api.bake_bitmap(k_sess, bmp, &bake).id, asset_none.id);
}

TEST_CASE("shader api: bake_filmstrip sweeps a uniform across frames")
{
  Fixture f;
  neui_asset_t a = f.create(k_ring_glow);
  const neui_shader_uniform_t accent = neui_shader_color("accent", 0xFF3FA9F5u);
  neui_shader_bake_t bake{};
  bake.width = 16.0f;
  bake.height = 16.0f;
  bake.uniforms = &accent;
  bake.uniform_count = 1;

  neui_asset_t s = f.api.bake_filmstrip(k_sess, a, &bake, "value", 0.0f, 1.0f, 5);
  REQUIRE(s.id != asset_none.id);
  AssetEntry* e = f.entry(s);
  REQUIRE(e != nullptr);
  CHECK_EQ(e->kind, NEUI_ASSET_KIND_BITMAP);
  // 5 cells of 16 px: 2 x 3 keeps the longer side shortest (48 px).
  CHECK_EQ(e->width_px, 32u);
  CHECK_EQ(e->height_px, 48u);
  const FilmstripInfo* fs = f.store.frame_info(s.id & 0xffff);
  REQUIRE(fs != nullptr);
  CHECK_EQ(fs->frame_count, 5u);
  CHECK_EQ(f.store.frame_count(s.id & 0xffff), 5u);
  CHECK_EQ(fs->cols, 2u);
  CHECK_EQ(fs->rows, 3u);
  CHECK_EQ(fs->frame_w_px, 16u);
  CHECK_EQ(fs->frame_h_px, 16u);

  // Frame i is the bake at value i / 4, in the cell the draw path picks.
  for (uint32_t i = 0; i < 5; ++i) {
    neui_shader_uniform_t two[2] = { accent, { "value", { nsl::sweep_value(0.0f, 1.0f, i, 5), 0, 0, 0 } } };
    neui_shader_bake_t one = bake;
    one.uniforms = two;
    one.uniform_count = 2;
    neui_asset_t b = f.api.bake_bitmap(k_sess, a, &one);
    REQUIRE(b.id != asset_none.id);
    e = f.entry(s);
    float sx, sy, sw, sh;
    filmstrip_src_rect(fs->frame_count, fs->cols, fs->rows, e->width_px, e->height_px, 0, i,
                       sx, sy, sw, sh);
    CHECK_EQ(sw, 16.0f);
    CHECK_EQ(sh, 16.0f);
    CHECK(cell(*e, 16, 16, static_cast<uint32_t>(sx) / 16, static_cast<uint32_t>(sy) / 16) ==
          f.entry(b)->pixels);
  }
  // The unused sixth cell stays transparent.
  e = f.entry(s);
  const std::vector<uint8_t> empty = cell(*e, 16, 16, 1, 2);
  CHECK(std::all_of(empty.begin(), empty.end(), [](uint8_t v) { return v == 0; }));

  // The ends are exact.
  CHECK_EQ(nsl::sweep_value(0.1f, 0.3f, 0, 7), 0.1f);
  CHECK_EQ(nsl::sweep_value(0.1f, 0.3f, 6, 7), 0.3f);
  CHECK_EQ(nsl::sweep_value(0.1f, 0.3f, 0, 1), 0.1f);
}

TEST_CASE("shader api: bake_filmstrip can sweep neui_time")
{
  Fixture f;
  neui_asset_t a = f.create(k_clock);
  neui_shader_bake_t bake{};
  bake.width = 8.0f;
  bake.height = 8.0f;
  bake.time = 99.0f;  // ignored: the sweep sets the time
  neui_asset_t s = f.api.bake_filmstrip(k_sess, a, &bake, "neui_time", 0.0f, 3.0f, 4);
  REQUIRE(s.id != asset_none.id);
  AssetEntry* e = f.entry(s);
  REQUIRE(e != nullptr);
  CHECK_EQ(f.store.frame_count(s.id & 0xffff), 4u);
  const nsl::Program& prog = *f.entry(a)->shader->program;
  const FilmstripInfo* fs = f.store.frame_info(s.id & 0xffff);
  REQUIRE(fs != nullptr);
  for (uint32_t i = 0; i < 4; ++i)
    CHECK(cell(*e, 8, 8, i % fs->cols, i / fs->cols) ==
          run(prog, 8.0f, 8.0f, 1.0f, static_cast<float>(i), {}));
}

TEST_CASE("shader api: bake_filmstrip rejects a bad sweep")
{
  Fixture f;
  neui_asset_t a = f.create(k_ring_glow);
  neui_shader_bake_t bake{};
  bake.width = 8.0f;
  bake.height = 8.0f;
  CHECK_EQ(f.api.bake_filmstrip(k_sess, a, &bake, "accent", 0, 1, 4).id, asset_none.id);  // vec4
  CHECK_EQ(f.api.bake_filmstrip(k_sess, a, &bake, "nope", 0, 1, 4).id, asset_none.id);
  CHECK_EQ(f.api.bake_filmstrip(k_sess, a, &bake, "neui_size", 0, 1, 4).id, asset_none.id);
  CHECK_EQ(f.api.bake_filmstrip(k_sess, a, &bake, nullptr, 0, 1, 4).id, asset_none.id);
  CHECK_EQ(f.api.bake_filmstrip(k_sess, a, &bake, "value", 0, 1, 0).id, asset_none.id);
  // One frame is a strip of one, at `from`.
  neui_asset_t one = f.api.bake_filmstrip(k_sess, a, &bake, "value", 0.25f, 1.0f, 1);
  REQUIRE(one.id != asset_none.id);
  CHECK_EQ(f.store.frame_count(one.id & 0xffff), 1u);
  // More frames than fit 16384 px a side.
  bake.width = 4000.0f;
  bake.height = 4000.0f;
  CHECK_EQ(f.api.bake_filmstrip(k_sess, a, &bake, "value", 0, 1, 17).id, asset_none.id);
}

TEST_CASE("shader strip grid: the longer side is kept short")
{
  uint32_t c = 0, r = 0;
  REQUIRE(shader_strip_grid(1, 50, 50, 16384, c, r));
  CHECK_EQ(c, 1u);
  CHECK_EQ(r, 1u);
  // 128 knob frames of 240 px: 30720 px as one column; 11 x 12 is 2640 x 2880.
  REQUIRE(shader_strip_grid(128, 240, 240, 16384, c, r));
  CHECK_EQ(c, 11u);
  CHECK_EQ(r, 12u);
  // Wide, flat cells stack in one column.
  REQUIRE(shader_strip_grid(10, 2000, 10, 16384, c, r));
  CHECK_EQ(c, 1u);
  CHECK_EQ(r, 10u);
  // 64 x 64 frames fill 8 x 8 exactly.
  REQUIRE(shader_strip_grid(64, 48, 48, 16384, c, r));
  CHECK_EQ(c, 8u);
  CHECK_EQ(r, 8u);
  CHECK_FALSE(shader_strip_grid(1, 20000, 10, 16384, c, r));
  CHECK_FALSE(shader_strip_grid(300000000u, 1, 1, 16384, c, r));
  CHECK_FALSE(shader_strip_grid(0, 10, 10, 16384, c, r));
}

namespace {

struct ShaderFiles {
  std::vector<std::string>          names;
  std::vector<neui_resource_kind_t> kinds;
  std::string                       glow, bad;
};
ShaderFiles g_files;

bool NEUI_ABI files_provide(void* token, const neui_resource_request_t* req,
                            neui_resource_bytes_t* out)
{
  auto* fs = static_cast<ShaderFiles*>(token);
  fs->names.push_back(req->name);
  fs->kinds.push_back(req->kind);
  const std::string* src = nullptr;
  if (std::strcmp(req->name, "shaders/glow.nsl") == 0) src = &fs->glow;
  if (std::strcmp(req->name, "shaders/bad.nsl") == 0) src = &fs->bad;
  if (!src) return false;
  out->data = reinterpret_cast<const uint8_t*>(src->data());
  out->len = static_cast<uint32_t>(src->size());
  return true;
}

}  // namespace

TEST_CASE("shader api: create_from_file reads through the resource provider")
{
  Fixture f;
  g_files = ShaderFiles{};
  g_files.glow = k_ring_glow;
  g_files.bad = "vec4 main(vec2 p) { return 1.0; }";
  neui_resource_client_t client{};
  client.kinds_mask = NEUI_RESOURCE_MASK_SHADER;
  client.provide = files_provide;
  ResourceProvider provider;
  provider.client = &client;
  provider.token = &g_files;
  f.store.set_resource_provider(provider);

  char err[256];
  neui_asset_t a = f.api.create_from_file(k_sess, "shaders/glow.nsl", err, sizeof err);
  REQUIRE(a.id != asset_none.id);
  CHECK_EQ(f.entry(a)->kind, NEUI_ASSET_KIND_SHADER);
  REQUIRE(g_files.kinds.size() == 1u);
  CHECK_EQ(g_files.kinds[0], NEUI_RESOURCE_KIND_SHADER);
  CHECK_EQ(g_files.names[0], std::string("shaders/glow.nsl"));

  // The path labels the diagnostic.
  CHECK_EQ(f.api.create_from_file(k_sess, "shaders/bad.nsl", err, sizeof err).id, asset_none.id);
  CHECK(std::string(err).rfind("shaders/bad.nsl:1:", 0) == 0);

  // Neither the client nor the filesystem has it.
  CHECK_EQ(f.api.create_from_file(k_sess, "no/such/shader.nsl", err, sizeof err).id, asset_none.id);
  CHECK_EQ(std::string(err), std::string("no/such/shader.nsl: cannot read the file"));
  CHECK_EQ(f.api.create_from_file(k_sess, nullptr, err, sizeof err).id, asset_none.id);
}

// The same bytes on every platform: this pins the bake path (sweep values,
// time wrap, strip layout) on top of the executor goldens in
// test_shader_exec.cpp.
TEST_CASE("shader api: filmstrip golden")
{
  Fixture f;
  neui_asset_t a = f.create(k_ring_glow);
  const neui_shader_uniform_t accent = neui_shader_color("accent", 0xC03FA9F5u);
  neui_shader_bake_t bake{};
  bake.width = 24.0f;
  bake.height = 24.0f;
  bake.scale = 1.5f;
  bake.uniforms = &accent;
  bake.uniform_count = 1;
  neui_asset_t s = f.api.bake_filmstrip(k_sess, a, &bake, "value", 0.05f, 0.95f, 7);
  REQUIRE(s.id != asset_none.id);
  AssetEntry* e = f.entry(s);
  CHECK_EQ(e->width_px, 108u);
  CHECK_EQ(e->height_px, 108u);
  CHECK_EQ(crc32(e->pixels.data(), e->pixels.size()), 0x20180be7u);

  neui_asset_t c = f.create(k_clock);
  bake.uniform_count = 0;
  bake.scale = 1.0f;
  neui_asset_t t = f.api.bake_filmstrip(k_sess, c, &bake, "neui_time", 3590.0f, 3610.0f, 9);
  REQUIRE(t.id != asset_none.id);
  e = f.entry(t);
  CHECK_EQ(crc32(e->pixels.data(), e->pixels.size()), 0xe5fcfb71u);
}
