#include "neui_test.h"

// .nsl texture sampling (plans/shader-effects.md, phase 9): sampler2D
// uniforms, texture2D / neui_texture_size / neui_texel, the sampler pragma,
// the exact filter arithmetic (a bitmap drawn 1:1 comes back byte for byte),
// edge modes, unbound samplers, hoisting, and cross-platform goldens.
#include "shader/nsl.h"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <limits>
#include <string>
#include <vector>

using namespace neui_test;
namespace nsl = neui::nsl;

namespace {

nsl::CompileResult compile(const std::string& src) { return nsl::compile(src.data(), src.size(), "t.nsl"); }

std::shared_ptr<const nsl::Program> build(const std::string& src) {
  nsl::CompileResult r = compile(src);
  if (!r.program) std::fprintf(stderr, "  compile error: %s\n", r.error.c_str());
  return r.program;
}

bool fails_at(const std::string& src, int line, int col, const char* msg) {
  const nsl::CompileResult r = compile(src);
  const bool ok = !r.program && r.line == line && r.col == col && r.error.find(msg) != std::string::npos;
  if (!ok)
    std::fprintf(stderr, "  expected %d:%d '%s', got %s\n", line, col, msg, r.program ? "success" : r.error.c_str());
  return ok;
}

uint32_t crc32(const void* data, size_t len) {
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
  uint32_t crc = ~0u;
  const uint8_t* p = static_cast<const uint8_t*>(data);
  for (size_t i = 0; i < len; ++i) crc = table[(crc ^ p[i]) & 0xffu] ^ (crc >> 8);
  return ~crc;
}

// A deterministic premultiplied BGRA bitmap: every alpha, colour never above
// it, fully transparent texels included.
struct Bitmap {
  int w = 0, h = 0;
  float scale = 1.0f;
  std::vector<uint8_t> px;
  nsl::Texture tex() const {
    nsl::Texture t;
    t.bgra = px.data();
    t.w = w;
    t.h = h;
    t.stride = static_cast<size_t>(w) * 4u;
    t.scale = scale;
    return t;
  }
};

Bitmap make_bitmap(int w, int h, float scale = 1.0f) {
  Bitmap b;
  b.w = w;
  b.h = h;
  b.scale = scale;
  b.px.resize(static_cast<size_t>(w) * static_cast<size_t>(h) * 4u);
  uint32_t s = 0x9E3779B9u;
  for (size_t i = 0; i < b.px.size(); i += 4) {
    s = s * 1664525u + 1013904223u;
    const uint32_t a = (s >> 24) % 5u == 0 ? 0u : (s >> 24);  // some fully transparent
    for (int c = 0; c < 3; ++c) {
      s = s * 1664525u + 1013904223u;
      b.px[i + static_cast<size_t>(c)] = static_cast<uint8_t>((s >> 16) % (a + 1u));
    }
    b.px[i + 3] = static_cast<uint8_t>(a);
  }
  return b;
}

// Renders a draw (execute == execute_scalar checked).
std::vector<uint8_t> render(const nsl::Program& p, const nsl::Draw& d) {
  const nsl::Placement pl = nsl::placement(d);
  std::vector<uint8_t> a(static_cast<size_t>(pl.w) * static_cast<size_t>(pl.h) * 4u), b(a.size());
  const size_t stride = static_cast<size_t>(pl.w) * 4u;
  CHECK(nsl::execute(p, d, a.data(), stride));
  CHECK(nsl::execute_scalar(p, d, b.data(), stride));
  CHECK(a == b);
  return a;
}

// main at one point (SIMD == scalar checked).
std::vector<float> eval1(const nsl::Program& p, const nsl::Draw& d, float x, float y) {
  std::vector<float> simd(4), scalar(4);
  nsl::evaluate(p, d, &x, &y, 1, simd.data(), true);
  nsl::evaluate(p, d, &x, &y, 1, scalar.data(), false);
  CHECK(std::memcmp(simd.data(), scalar.data(), 16) == 0);
  return simd;
}

nsl::Draw draw_with(const nsl::Texture* t, int n, float w, float h, float scale = 1.0f) {
  nsl::Draw d;
  d.w = w;
  d.h = h;
  d.scale = scale;
  d.textures = t;
  d.texture_count = n;
  return d;
}

const char* const k_copy = R"(
uniform sampler2D img;
vec4 main(vec2 p) { return texture2D(img, p / neui_size); }
)";

}  // namespace

TEST_CASE("shader texture: declarations and introspection")
{
  auto p = build(R"(
uniform float level = 0.5;
uniform sampler2D albedo;
#pragma neui sampler noise repeat nearest
uniform lowp sampler2D noise;
#pragma neui sampler albedo border
vec4 main(vec2 p) { return texture2D(albedo, p) * level + texture(noise, p); }
)");
  REQUIRE(p != nullptr);
  REQUIRE(nsl::uniforms(*p).size() == 1);
  CHECK(nsl::uniforms(*p)[0].name == "level");
  CHECK(nsl::uniform_components(*p) == 1);
  REQUIRE(nsl::samplers(*p).size() == 2);
  CHECK(nsl::samplers(*p)[0].name == "albedo");
  CHECK(nsl::samplers(*p)[0].wrap == nsl::Wrap::BORDER);
  CHECK(nsl::samplers(*p)[0].filter == nsl::Filter::LINEAR);
  CHECK(nsl::samplers(*p)[1].name == "noise");
  CHECK(nsl::samplers(*p)[1].wrap == nsl::Wrap::REPEAT);
  CHECK(nsl::samplers(*p)[1].filter == nsl::Filter::NEAREST);
  CHECK(nsl::stats(*p).fetches == 4 + 1);  // texel reads: bilinear 4, nearest 1
  // No GPU form until the D2D path samples.
  CHECK(nsl::emit_hlsl(*p).empty());
  CHECK(nsl::gpu_constant_count(*p) == 0);
}

TEST_CASE("shader texture: diagnostics")
{
  const std::string m = "vec4 main(vec2 p) { return vec4(0.0); }\n";
  CHECK(fails_at("uniform sampler2D t = 1.0;\n" + m, 1, 21, "a sampler2D has no default"));
  CHECK(fails_at("uniform sampler2D a;\nuniform sampler2D b;\nuniform sampler2D c;\nuniform sampler2D d;\n"
                 "uniform sampler2D e;\n" + m, 5, 19, "more than 4 sampler2D uniforms"));
  CHECK(fails_at("sampler2D t;\n" + m, 1, 1, "a sampler2D is a uniform or an 'in' parameter"));
  CHECK(fails_at("uniform sampler2D t;\nvec4 main(vec2 p) {\n  sampler2D s;\n  return vec4(0.0);\n}\n", 3, 3,
                 "a sampler2D is a uniform or an 'in' parameter"));
  CHECK(fails_at("uniform sampler2D t;\nvec4 main(vec2 p) {\n  return vec4(t + 1.0);\n}\n", 3, 17,
                 "cannot apply '+' to sampler2D and float"));
  CHECK(fails_at("uniform sampler2D t;\nvec4 main(vec2 p) {\n  if (t == t) return vec4(1.0);\n  return vec4(0.0);\n}\n",
                 3, 9, "cannot compare sampler2D"));
  CHECK(fails_at("uniform sampler2D t;\nuniform sampler2D u;\nvec4 main(vec2 p) {\n"
                 "  return texture2D(p.x > 0.0 ? t : u, p);\n}\n", 4, 30, "'?:' cannot select a sampler2D"));
  CHECK(fails_at("float f(out sampler2D s) { return 0.0; }\n" + m, 1, 13, "sampler2D parameters must be 'in'"));
  CHECK(fails_at("float f(sampler2D s) {\n  s = s;\n  return 0.0;\n}\n" + m, 2, 3, "sampler2D 's' cannot be assigned"));
  CHECK(fails_at("uniform sampler2D t;\nvec4 main(vec2 p) {\n  return texture2D(t, 1.0);\n}\n", 3, 10,
                 "'texture2D' takes a sampler2D and a vec2"));
  CHECK(fails_at("uniform sampler2D t;\nvec4 main(vec2 p) {\n  return vec4(neui_texel(p), 0.0, 0.0);\n}\n", 3, 15,
                 "'neui_texel' takes a sampler2D"));
  CHECK(fails_at("uniform sampler2D t;\nvec4 main(vec2 p) {\n  return vec4(sin(t));\n}\n", 3, 19,
                 "takes float or vector arguments, not sampler2D"));
  CHECK(fails_at("uniform sampler2D t;\nvec4 main(vec2 p) {\n  float x = t;\n  return vec4(x);\n}\n", 3, 13,
                 "'x' is float, but its value is sampler2D"));
  CHECK(fails_at("#pragma neui sampler nope clamp\n" + m, 1, 1, "'nope', which is not a sampler2D uniform"));
  CHECK(fails_at("uniform sampler2D t;\n#pragma neui sampler t clamp\n#pragma neui sampler t nearest\n" + m, 3, 1,
                 "has two '#pragma neui sampler' lines"));
  CHECK(fails_at("uniform sampler2D t;\n#pragma neui sampler t mirror\n" + m, 2, 1, "unknown sampler mode 'mirror'"));
  CHECK(fails_at("uniform sampler2D t;\n#pragma neui sampler t clamp repeat\n" + m, 2, 1, "sets the edge mode twice"));
  CHECK(fails_at("#pragma neui sampler\n" + m, 1, 1, "takes a sampler name"));
  CHECK(fails_at("uniform sampler3D t;\n" + m, 1, 9, "'sampler3D' is not supported in .nsl v1"));
  // 32 uniform declarations in all, samplers included.
  std::string many;
  for (int i = 0; i < 31; ++i) many += "uniform float u" + std::to_string(i) + ";\n";
  CHECK(build(many + "uniform sampler2D t;\n" + m) != nullptr);
  CHECK(fails_at(many + "uniform sampler2D t;\nuniform sampler2D s;\n" + m, 33, 19, "more than 32 uniforms"));
}

TEST_CASE("shader texture: a bitmap drawn 1:1 comes back byte for byte")
{
  for (const char* filter : {"linear", "nearest"}) {
    for (float scale : {1.0f, 2.0f, 4.0f}) {
      const std::string src = std::string("#pragma neui sampler img ") + filter + "\n" + k_copy;
      auto p = build(src);
      REQUIRE(p != nullptr);
      const Bitmap bm = make_bitmap(37, 23, scale);
      const nsl::Texture t = bm.tex();
      const nsl::Draw d = draw_with(&t, 1, 37.0f / scale, 23.0f / scale, scale);
      const std::vector<uint8_t> out = render(*p, d);
      CHECK(out == bm.px);
    }
  }
}

TEST_CASE("shader texture: sampler parameters and the texture alias")
{
  auto direct = build(k_copy);
  auto via_fn = build(R"(
uniform sampler2D img;
vec4 tap(sampler2D s, vec2 uv) { return texture(s, uv); }
vec4 main(vec2 p) { return tap(img, p / neui_size); }
)");
  REQUIRE(direct != nullptr);
  REQUIRE(via_fn != nullptr);
  const Bitmap bm = make_bitmap(16, 16);
  const nsl::Texture t = bm.tex();
  const nsl::Draw d = draw_with(&t, 1, 40.0f, 30.0f, 1.5f);
  CHECK(render(*direct, d) == render(*via_fn, d));
}

TEST_CASE("shader texture: edge modes, filters and unbound samplers")
{
  // A 4x1 strip: texel i has alpha 50 * (i + 1) and colour = alpha (white).
  Bitmap bm;
  bm.w = 4;
  bm.h = 1;
  for (int i = 0; i < 4; ++i)
    for (int c = 0; c < 4; ++c) bm.px.push_back(static_cast<uint8_t>(50 * (i + 1)));
  const nsl::Texture t = bm.tex();
  const nsl::Draw d = draw_with(&t, 1, 1.0f, 1.0f);
  auto alpha_at = [&](const char* modes, float u) {
    auto p = build(std::string("#pragma neui sampler s ") + modes +
                   "\nuniform sampler2D s;\nvec4 main(vec2 p) { return texture2D(s, p); }\n");
    return p ? eval1(*p, d, u, 0.5f)[3] * 255.0f : -1.0f;
  };
  auto near = [](float a, float b) { return std::fabs(a - b) < 1e-3f; };
  // Nearest: texel floor(u * 4).
  CHECK(near(alpha_at("nearest", 0.30f), 100.0f));
  CHECK(near(alpha_at("clamp nearest", -0.1f), 50.0f));
  CHECK(near(alpha_at("repeat nearest", -0.1f), 200.0f));
  CHECK(near(alpha_at("border nearest", -0.1f), 0.0f));
  CHECK(near(alpha_at("clamp nearest", 1.1f), 200.0f));
  CHECK(near(alpha_at("repeat nearest", 1.1f), 50.0f));
  CHECK(near(alpha_at("border nearest", 1.1f), 0.0f));
  CHECK(alpha_at("repeat nearest", 1e20f) > 49.0f);  // whatever texel, no crash
  CHECK(near(alpha_at("clamp nearest", 1e20f), 200.0f));
  CHECK(near(alpha_at("clamp nearest", -1e20f), 50.0f));
  CHECK(near(alpha_at("clamp nearest", std::numeric_limits<float>::quiet_NaN()), 50.0f));
  // Linear: halfway between texel centres 0 and 1 (u = 0.25), and a quarter.
  CHECK(near(alpha_at("linear", 0.25f), 75.0f));
  CHECK(near(alpha_at("linear", 0.1875f), 62.5f));
  // Repeat blends the last texel with the first across the seam.
  CHECK(near(alpha_at("repeat", 0.0f), 125.0f));
  CHECK(near(alpha_at("clamp", 0.0f), 50.0f));
  CHECK(near(alpha_at("border", 0.0f), 25.0f));
  // Straight colour: white stays white however transparent.
  {
    auto p = build("#pragma neui sampler s border\nuniform sampler2D s;\nvec4 main(vec2 p) { return texture2D(s, p); }\n");
    REQUIRE(p != nullptr);
    const std::vector<float> v = eval1(*p, d, 0.0f, 0.5f);
    CHECK(v[0] == 1.0f);
    CHECK(v[1] == 1.0f);
    CHECK(v[2] == 1.0f);
  }
  // Unbound: no table, a null bitmap, a zero size, a bad stride.
  auto p = build(R"(
uniform sampler2D s;
vec4 main(vec2 p) { return texture2D(s, p) + vec4(neui_texture_size(s), neui_texel(s)); }
)");
  REQUIRE(p != nullptr);
  nsl::Texture bad[4] = {nsl::Texture(), t, t, t};
  bad[1].w = 0;
  bad[2].stride = 4;
  bad[3].bgra = nullptr;
  for (int k = 0; k < 5; ++k) {
    const nsl::Draw du = k == 4 ? draw_with(nullptr, 0, 1.0f, 1.0f) : draw_with(&bad[k], 1, 1.0f, 1.0f);
    const std::vector<float> v = eval1(*p, du, 0.5f, 0.5f);
    CHECK(v[0] == 0.0f);
    CHECK(v[1] == 0.0f);
    CHECK(v[2] == 0.0f);
    CHECK(v[3] == 0.0f);
  }
}

TEST_CASE("shader texture: sizes, and a uniform-only sample runs once per draw")
{
  const Bitmap bm = make_bitmap(64, 32, 2.0f);
  const nsl::Texture t = bm.tex();
  const nsl::Draw d = draw_with(&t, 1, 10.0f, 10.0f);
  auto sizes = build(R"(
uniform sampler2D s;
vec4 main(vec2 p) { return vec4(neui_texture_size(s), neui_texel(s)); }
)");
  REQUIRE(sizes != nullptr);
  const std::vector<float> v = eval1(*sizes, d, 1.0f, 1.0f);
  CHECK(v[0] == 32.0f);
  CHECK(v[1] == 16.0f);
  CHECK(v[2] == 1.0f / 64.0f);
  CHECK(v[3] == 1.0f / 32.0f);
  CHECK(nsl::stats(*sizes).pixel_ops == 0);

  auto hoisted = build(R"(
uniform float u = 0.3;
uniform sampler2D s;
vec4 main(vec2 p) { return texture2D(s, vec2(u, 0.6)); }
)");
  auto varying = build(R"(
uniform sampler2D s;
vec4 main(vec2 p) { return texture2D(s, p); }
)");
  REQUIRE(hoisted != nullptr);
  REQUIRE(varying != nullptr);
  CHECK(nsl::stats(*hoisted).fetches == 0);
  CHECK(nsl::stats(*hoisted).pixel_ops == 0);
  CHECK(nsl::stats(*varying).fetches == 4);
  const std::vector<float> a = eval1(*hoisted, d, 7.0f, 3.0f);
  const std::vector<float> b = eval1(*varying, d, 0.3f, 0.6f);
  CHECK(std::memcmp(a.data(), b.data(), 16) == 0);
}

TEST_CASE("shader texture: goldens")
{
  // Magnified, minified and offset sampling under every mode, at fractional
  // scales and origins. The checksums must match on every platform.
  const Bitmap bm = make_bitmap(13, 9, 1.0f);
  const nsl::Texture t = bm.tex();
  struct Case {
    const char* modes;
    float x, y, w, h, scale;
    uint32_t crc;
  };
  const Case cases[] = {
    {"clamp linear", 0.0f, 0.0f, 64.0f, 48.0f, 1.0f, 0xad549d9au},
    {"repeat linear", 0.3f, 0.7f, 40.0f, 40.0f, 1.5f, 0xabf9d02cu},
    {"border linear", 0.0f, 0.0f, 30.0f, 20.0f, 2.0f, 0x5a51932fu},
    {"clamp nearest", 0.0f, 0.0f, 64.0f, 48.0f, 1.0f, 0x2b2f1436u},
    {"repeat nearest", 0.25f, 0.5f, 7.0f, 5.0f, 1.25f, 0x9dcdd090u},
    {"border nearest", 0.0f, 0.0f, 30.0f, 20.0f, 2.0f, 0x0b30d4e0u},
  };
  for (const Case& c : cases) {
    auto p = build(std::string("#pragma neui sampler s ") + c.modes + R"(
uniform sampler2D s;
vec4 main(vec2 p) {
  vec2 uv = (p - neui_size * 0.5) / neui_size * 1.6 + 0.5;
  vec4 a = texture2D(s, uv);
  vec4 b = texture2D(s, uv.yx * 3.0);
  return mix(a, b, 0.25);
})");
    REQUIRE(p != nullptr);
    nsl::Draw d = draw_with(&t, 1, c.w, c.h, c.scale);
    d.x = c.x;
    d.y = c.y;
    const std::vector<uint8_t> out = render(*p, d);
    const uint32_t crc = crc32(out.data(), out.size());
    if (crc != c.crc) std::fprintf(stderr, "  golden %s: 0x%08xu\n", c.modes, crc);
    CHECK(crc == c.crc);
  }
}
