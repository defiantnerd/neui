#include "neui_test.h"

// The .nsl compiler front end (src/shader/nsl.h): what compiles, what does
// not and where the diagnostic points, the limits of docs/shader-language.md
// section 13, literal rounding, and mutation fuzzing.
#include "shader/nsl.h"

#include <clocale>
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

using namespace neui_test;
namespace nsl = neui::nsl;

namespace {

nsl::CompileResult compile(const std::string& src, const char* name = "t.nsl") {
  return nsl::compile(src.data(), src.size(), name);
}

bool compiles(const std::string& src) {
  const nsl::CompileResult r = compile(src);
  if (!r.program) std::fprintf(stderr, "  unexpected error: %s\n", r.error.c_str());
  return r.program != nullptr;
}

// Expects a compile error at line:col whose message contains `msg`.
bool fails_at(const std::string& src, int line, int col, const char* msg) {
  const nsl::CompileResult r = compile(src);
  const bool ok = !r.program && r.line == line && r.col == col && r.error.find(msg) != std::string::npos;
  if (!ok)
    std::fprintf(stderr, "  expected %d:%d '%s', got %s\n", line, col, msg,
                 r.program ? "success" : r.error.c_str());
  return ok;
}

// Expects a compile error whose message contains `msg`, anywhere.
bool fails_with(const std::string& src, const char* msg) {
  const nsl::CompileResult r = compile(src);
  const bool ok = !r.program && r.error.find(msg) != std::string::npos;
  if (!ok) std::fprintf(stderr, "  expected '%s', got %s\n", msg, r.program ? "success" : r.error.c_str());
  return ok;
}

std::string with_main(const std::string& body) { return "vec4 main(vec2 p) {\n" + body + "\n}"; }

// Every positive program: the fuzzer's corpus too.
const char* const k_corpus[] = {
  // The spec's example.
  R"(// ring_glow.nsl - a value-driven glow ring for a knob
uniform float value = 0.5;              // 0..1, bound to neui.param.value
uniform vec4  accent = vec4(0.2, 0.7, 1.0, 1.0);

vec4 main(vec2 p) {
  vec2  c    = neui_size * 0.5;
  float r    = min(c.x, c.y) * 0.8;
  float d    = abs(length(p - c) - r);  // distance to the ring centreline
  float ring = 1.0 - smoothstep(0.0, 1.5 * neui_px, d - 2.0);
  float glow = exp(-d * mix(0.6, 0.15, value));
  float a    = saturate(ring + glow * value);
  return vec4(accent.rgb, a * accent.a);
})",
  // Functions with out / inout, early returns, swizzle writes.
  R"(precision highp float;
const float PI = 3.14159265;
const vec2 CENTRE = vec2(16.0, 16.0), ZERO = vec2(0.0);
void polar(in vec2 v, out float radius, out float angle) { radius = length(v); angle = atan(v.y, v.x); }
void tint(inout vec3 c, float k) { c.rb *= k; c.g = c.g + 0.1; }
float band(float x, float lo, float hi) { if (x < lo) return 0.0; else if (x > hi) return 0.0; return 1.0; }
vec4 main(in highp vec2 pos) {
  float r; float a;
  polar(pos - CENTRE, r, a);
  vec3 col = vec3(0.5 + 0.5 * cos(a + PI * vec3(0.0, 0.66, 1.33)));
  tint(col, band(r, 4.0, 12.0));
  col.xy = col.yx;
  return vec4(col, 1.0);
})",
  // Loops with break / continue, nested, const int bounds.
  R"(const int N = 8;
vec4 main(vec2 p) {
  float acc = 0.0;
  for (int i = 0; i < N; i++) {
    if (acc > 3.0) break;
    for (int j = N; j > 0; j -= 2) {
      if (mod(float(j), 4.0) == 0.0) continue;
      acc += sin(p.x * 0.1 + float(i * j)) * 0.25;
    }
  }
  for (int k = 0; k <= 3; ++k) acc *= 0.9;
  for (int k = 10; k != 0; --k) acc += 0.0;
  return vec4(vec3(acc), 1.0);
})",
  // Booleans, ternaries, comparisons, every built-in.
  R"(#pragma neui dither
uniform float t;
vec4 main(vec2 p) {
  bool inside = p.x > 2.0 && p.y < 30.0 || !(p.x == p.y) ^^ false;
  float a = inside ? 1.0 : 0.0;
  vec2 v = vec2(abs(-1.0), sign(p.x)) + floor(p) + ceil(p) + fract(p);
  float b = sqrt(4.0) + inversesqrt(4.0) + sin(t) + cos(t) + tan(0.1) + asin(0.5) + acos(0.5);
  float c = exp(1.0) + log(2.0) + exp2(3.0) + log2(8.0) + radians(90.0) + degrees(1.0);
  float d = saturate(1.5) + atan(1.0) + atan(1.0, 2.0) + mod(7.0, 3.0) + min(1, 2.0) + max(p.x, 3);
  float e = step(0.5, p.x) + pow(2.0, 3.0) + clamp(p.x, 0.0, 1.0) + mix(0.0, 1.0, 0.5);
  float f = smoothstep(0.0, 1.0, 0.5) + length(p) + distance(p, vec2(1.0)) + dot(p, p);
  vec3 g = normalize(vec3(1.0, 2.0, 3.0)) + cross(vec3(1.0, 0.0, 0.0), vec3(0.0, 1.0, 0.0));
  vec2 h = reflect(v, vec2(0.0, 1.0)) + vec2(neui_hash(p), neui_noise(p));
  vec4 q = vec4(vec2(a, b), c, d) + vec4(e, f, g.x, h.y) + vec4(neui_scale, neui_time, neui_size);
  return vec4(q.xyz * 0.01, q.w > 0.0 ? 1.0 : 0.5);
})",
  // Uniform-only work, shadowing, empty statements, unary chains.
  "uniform vec3 base = vec3(1, 2, 3);\r\n"
  "vec4 main(vec2 p) {\r\n"
  "  vec3 k = base * 2.0 / 3.0;;\r\n"
  "  { float base = - - 1.0; k.x += base; }\r\n"
  "  float f = 1.5e-3 + .5 + 2. + 1e2 + 3.0f;\r\n"
  "  return vec4(k, f) * 0.1;\r\n"
  "}\r\n",
};

}  // namespace

TEST_CASE("shader lang: the corpus compiles")
{
  for (const char* src : k_corpus) CHECK(compiles(src));
  // A byte-order mark is skipped.
  CHECK(compiles(std::string("\xEF\xBB\xBF") + k_corpus[0]));
}

TEST_CASE("shader lang: diagnostics point at the offending token")
{
  // The spec's three examples (docs/shader-language.md section 15).
  CHECK(fails_at(with_main("  return vec4(value);"), 2, 15, "'value' is not declared"));
  CHECK(fails_at(with_main("  float x = 1 / 2;\n  return vec4(x);"), 2, 15, "int division; write 1.0 / 2.0"));
  CHECK(fails_at(with_main("  float s = 0.0;\n  for (int i = 0; i < 128; i++) s += 1.0;\n  return vec4(s);"), 3, 3,
                 "loop runs 128 times; the limit is 64"));
  // Format: <name>:<line>:<col>: error: <message>
  const nsl::CompileResult r = nsl::compile("x", 1, "knob.nsl");
  CHECK(r.error.rfind("knob.nsl:1:1: error: ", 0) == 0);
  const nsl::CompileResult r2 = nsl::compile("x", 1, nullptr);
  CHECK(r2.error.rfind("<source>:1:1: error: ", 0) == 0);
}

TEST_CASE("shader lang: structure errors")
{
  CHECK(fails_with("float f(float x) { return x; }", "no 'vec4 main(vec2 p)' is defined"));
  CHECK(fails_at("vec3 main(vec2 p) { return vec3(0.0); }", 1, 6, "main must be declared as 'vec4 main(vec2 p)'"));
  CHECK(fails_at("vec4 main(vec2 p, float k) { return vec4(0.0); }", 1, 6, "main must be declared"));
  CHECK(fails_at("vec4 main(vec2 p) { return vec4(f(1.0)); }\nfloat f(float x) { return x; }", 1, 33,
                 "functions must be defined before they are called"));
  CHECK(fails_at("float f(float x) { return x; }\nfloat f(vec2 x) { return x.x; }\n" + with_main("return vec4(0.0);"),
                 2, 7, "cannot be overloaded"));
  CHECK(fails_at("float f(float x) { if (x > 0.0) return 1.0; }\n" + with_main("return vec4(f(p.x));"), 1, 7,
                 "does not return a value on every path"));
  CHECK(fails_at("float g = 1.0;\n" + with_main("return vec4(g);"), 1, 7, "global variables must be 'const' or 'uniform'"));
  CHECK(fails_at(with_main("  return vec4(main(p));"), 2, 15, "main cannot be called"));
  CHECK(fails_at("void f() { return 1.0; }\n" + with_main("f(); return vec4(0.0);"), 1, 19, "returns void"));
  CHECK(fails_at(with_main("  break;"), 2, 3, "'break' outside a loop"));
}

TEST_CASE("shader lang: lexical errors")
{
  CHECK(fails_at(with_main("  /* never closed"), 2, 3, "unterminated comment"));
  CHECK(fails_at(with_main("  float \xC3\xA4 = 1.0;"), 2, 9, "non-ASCII character outside a comment"));
  CHECK(compiles(with_main("  // caf\xC3\xA9 is fine in a comment\n  return vec4(0.0);")));
  CHECK(fails_at("#define K 1.0\n" + with_main("return vec4(0.0);"), 1, 1, "the preprocessor is not supported"));
  CHECK(fails_at("#pragma neui sparkle\n" + with_main("return vec4(0.0);"), 1, 1, "unknown pragma 'sparkle'"));
  CHECK(fails_at(with_main("  while (true) {}"), 2, 3, "'while' is not supported in .nsl v1"));
  CHECK(fails_at(with_main("  mat2 m;"), 2, 3, "'mat2' is not supported in .nsl v1"));
  CHECK(fails_at(with_main("  float x = 5.0 % 2.0;"), 2, 17, "use mod()"));
  CHECK(fails_at(with_main("  float a[2];"), 2, 10, "arrays are not supported"));
  CHECK(fails_at(with_main("  gl_FragColor = vec4(1.0);"), 2, 3, "'gl_FragColor' is reserved"));
  CHECK(fails_at(with_main("  float a__b = 1.0;"), 2, 9, "names containing __"));
  CHECK(fails_at(with_main("  float x = 2147483648;"), 2, 13, "integer literal is too large"));
  CHECK(fails_at(with_main("  float x = 1e39;"), 2, 13, "float literal is too large"));
  CHECK(fails_at(with_main("  float x = 1.0.5;"), 2, 13, "malformed number"));
  CHECK(fails_at(with_main("  float " + std::string(64, 'a') + " = 1.0;"), 2, 9, "longer than 63 characters"));
  CHECK(compiles(with_main("  float " + std::string(63, 'a') + " = 1.0;\n  return vec4(0.0);")));
}

TEST_CASE("shader lang: names and declarations")
{
  CHECK(fails_at(with_main("  float neui_x = 1.0;"), 2, 9, "names starting with neui_"));
  CHECK(fails_at(with_main("  float sin = 1.0;"), 2, 9, "'sin' is a built-in function"));
  CHECK(fails_at(with_main("  float x = 1.0;\n  float x = 2.0;"), 3, 9, "'x' is already declared"));
  CHECK(compiles(with_main("  float x = 1.0;\n  { float x = 2.0; }\n  return vec4(x);")));
  CHECK(fails_at(with_main("  int n = 3;"), 2, 3, "int variables must be const"));
  CHECK(fails_at(with_main("  const float k = p.x;\n  return vec4(k);"), 2, 19, "'k' needs a constant value"));
  CHECK(fails_at("uniform float a = 1.0;\nuniform float b = a;\n" + with_main("return vec4(b);"), 2, 19,
                 "uniform 'a' is not a constant"));
  CHECK(fails_at("uniform int n;\n" + with_main("return vec4(0.0);"), 1, 9, "uniforms are float, vec2, vec3, vec4 or sampler2D"));
  CHECK(fails_at("uniform float u;\n" + with_main("  u = 1.0;\n  return vec4(u);"), 3, 3, "uniform 'u' cannot be assigned"));
  CHECK(fails_at(with_main("  neui_time = 1.0;"), 2, 3, "'neui_time' cannot be assigned"));
  CHECK(fails_at(with_main("  for (int i = 0; i < 4; i++) { i = 2; }"), 2, 33, "loop counter 'i' cannot be assigned"));
  CHECK(fails_at("const float K = 1.0;\n" + with_main("  K = 2.0;"), 3, 3, "const 'K' cannot be assigned"));
}

TEST_CASE("shader lang: type errors")
{
  CHECK(fails_at(with_main("  float x = vec2(1.0);"), 2, 13, "'x' is float, but its value is vec2"));
  CHECK(fails_at(with_main("  vec3 v = vec3(1.0, 2.0);"), 2, 12, "vec3(...) needs 3 components, got 2"));
  CHECK(fails_at(with_main("  bool b = p < vec2(1.0);"), 2, 14, "compares scalars only"));
  CHECK(fails_at(with_main("  float x = true + 1.0;"), 2, 18, "cannot apply '+' to bool and float"));
  CHECK(fails_at(with_main("  vec3 v = vec3(1.0) + vec2(1.0);"), 2, 22, "cannot apply '+' to vec3 and vec2"));
  CHECK(fails_at(with_main("  float x = p.z;"), 2, 15, "vec2 has no component 'z'"));
  CHECK(fails_at(with_main("  float x = p.xg.x;"), 2, 15, "mixes component sets"));
  CHECK(fails_at(with_main("  vec2 v = p;\n  v.xx = vec2(1.0);"), 3, 5, "cannot repeat a component"));
  CHECK(fails_at(with_main("  float x = p.x > 1.0 ? 1.0 : vec2(0.0);"), 2, 23, "the two sides of '?:' are float and vec2"));
  CHECK(fails_at(with_main("  if (p.x) {}"), 2, 7, "the condition is float, expected bool"));
  CHECK(fails_at(with_main("  float x = cross(p, p);"), 2, 13, "'cross' takes two vec3"));
  CHECK(fails_at(with_main("  float x = sin(1.0, 2.0);"), 2, 13, "'sin' takes 1 argument"));
  CHECK(fails_at(with_main("  float x = mix(vec2(1.0), vec3(1.0), 0.5);"), 2, 28, "'mix' mixes vec2 and vec3"));
  CHECK(fails_at("void f(out float r) { r = 1.0; }\n" + with_main("  f(1.0);"), 3, 5, "is 'out' and needs a variable"));
  CHECK(fails_at(with_main("  float x = 1.0;\n  x *= vec2(2.0);"), 3, 5, "'*=' would make float a vec2"));
  CHECK(fails_at(with_main("  float x = 1.0;\n  x++;"), 3, 4, "only allowed in a for loop update"));
  CHECK(fails_at(with_main("  float x = int(1.0);"), 2, 13, "only float(...) and vecN(...)"));
  // Implicit int -> float, and the int rules.
  CHECK(compiles(with_main("  float x = 1 + 2.0 * 3;\n  vec2 v = vec2(1, 2) * 2;\n  return vec4(x, v, 1);")));
  CHECK(fails_at(with_main("  const int a = 2147483647;\n  const int b = a + 1;\n  return vec4(0.0);"), 3, 19, "int overflow"));
}

TEST_CASE("shader lang: loop rules")
{
  CHECK(fails_at(with_main("  for (float f = 0.0; f < 1.0; f += 0.1) {}"), 2, 8, "a for loop starts with 'int"));
  CHECK(fails_at(with_main("  for (int i = 0; i < 4; i += 0) {}\n  return vec4(0.0);"), 2, 3, "the loop step must be positive"));
  CHECK(fails_at(with_main("  for (int i = 0; i != 5; i += 2) {}\n  return vec4(0.0);"), 2, 3, "does not terminate"));
  CHECK(fails_at(with_main("  for (int i = 0; i < 10; i--) {}\n  return vec4(0.0);"), 2, 3, "does not terminate"));
  CHECK(fails_at(with_main("  float s = p.x;\n  for (int i = 0; i < int(s); i++) {}"), 3, 23, "only float(...) and vecN(...)"));
  CHECK(compiles(with_main("  float s = 0.0;\n  for (int i = 0; i < 64; i++) s += 1.0;\n  return vec4(s);")));
  // Nesting depth and the product of trip counts.
  std::string nest = "  float s = 0.0;\n";
  for (int d = 0; d < 5; ++d) nest += "  for (int i" + std::to_string(d) + " = 0; i" + std::to_string(d) + " < 2; i" + std::to_string(d) + "++)\n";
  nest += "  s += 1.0;\n  return vec4(s);";
  CHECK(fails_with(with_main(nest), "loops are nested more than 4 deep"));
  CHECK(fails_with(with_main("  float s = 0.0;\n  for (int i = 0; i < 64; i++) for (int j = 0; j < 17; j++) s += 1.0;\n  return vec4(s);"),
                   "nested loops run 1088 times in total; the limit is 1024"));
}

TEST_CASE("shader lang: size limits")
{
  CHECK(fails_at(std::string(64 * 1024 + 1, ' '), 1, 1, "source is larger than 64 KiB"));
  std::string deep = "  float x = ";
  for (int i = 0; i < 130; ++i) deep += "(";
  deep += "1.0";
  for (int i = 0; i < 130; ++i) deep += ")";
  deep += ";";
  CHECK(fails_with(with_main(deep), "nesting is deeper than 128"));
  std::string uni;
  for (int i = 0; i < 33; ++i) uni += "uniform float u" + std::to_string(i) + ";\n";
  CHECK(fails_with(uni + with_main("return vec4(0.0);"), "more than 32 uniforms"));
  std::string comps;
  for (int i = 0; i < 17; ++i) comps += "uniform vec4 c" + std::to_string(i) + ";\n";
  CHECK(fails_with(comps + with_main("return vec4(0.0);"), "uniforms need more than 64 floats"));
  std::string fns;
  for (int i = 0; i < 65; ++i) fns += "float f" + std::to_string(i) + "(float x) { return x; }\n";
  CHECK(fails_with(fns + with_main("return vec4(0.0);"), "more than 64 functions"));
  // More than 8192 operations after unrolling.
  CHECK(fails_with(with_main("  float s = p.x;\n"
                             "  for (int i = 0; i < 32; i++) for (int j = 0; j < 32; j++)\n"
                             "    s = sin(s * 1.01 + float(i * j)) * cos(s) + sqrt(abs(s)) + 0.5;\n"
                             "  return vec4(s);"),
                   "operations after inlining and unrolling; the limit is 8192"));
  // More than 256 values alive at once: every v is read by two sums, the
  // second in reverse, so all of them live across the first in any schedule.
  std::string many;
  for (int i = 0; i < 270; ++i) many += "  float v" + std::to_string(i) + " = p.x * " + std::to_string(i + 2) + ".0;\n";
  many += "  float s = 0.0;\n  float t = 0.0;\n";
  for (int i = 0; i < 270; ++i) many += "  s += v" + std::to_string(i) + ";\n";
  for (int i = 269; i >= 0; --i) many += "  t = t * 0.5 + v" + std::to_string(i) + ";\n";
  many += "  return vec4(s, t, 0.0, 1.0);";
  CHECK(fails_with(with_main(many), "live values per pixel after inlining and unrolling; the limit is 256"));
}

// Found by a code review: inputs within every source-level limit that made
// the compiler crash, overflow its stack or run for minutes.
TEST_CASE("shader lang: hostile programs fail quickly")
{
  // f_k calls f_{k-1} twice: 2^40 inlinings, while CSE keeps the IR tiny.
  std::string twice = "float f0(float x) { return x; }\n";
  for (int k = 1; k <= 40; ++k)
    twice += "float f" + std::to_string(k) + "(float x) { float t = f" + std::to_string(k - 1) +
             "(x); float u = f" + std::to_string(k - 1) + "(x); return t + u; }\n";
  CHECK(fails_with(twice + "vec4 main(vec2 p) { float t = f40(p.x); return vec4(t); }",
                   "shader is too large to compile"));

  // Braceless if / for bodies count toward the nesting limit.
  std::string ifs = "  float s = 0.0;\n  ";
  for (int i = 0; i < 3000; ++i) ifs += "if (p.x > 0.0) ";
  ifs += "s = 1.0;\n  return vec4(s);";
  CHECK(fails_with(with_main(ifs), "nesting is deeper than 128"));
  std::string elses = "  float s = 0.0;\n  if (p.x > 0.0) s = 1.0;\n";
  for (int i = 0; i < 1000; ++i) elses += "  else if (p.x > " + std::to_string(i) + ".0) s = 2.0;\n";
  elses += "  return vec4(s);";
  CHECK(fails_with(with_main(elses), "nesting is deeper than 128"));

  // Nesting adds up through inlining: 12 functions of 100 nested ifs each.
  std::string chain = "float g0(float x) { return x; }\n";
  for (int k = 1; k <= 12; ++k) {
    chain += "float g" + std::to_string(k) + "(float x) {\n  float t = x;\n  ";
    for (int i = 0; i < 100; ++i) chain += "if (x > 0.0) ";
    chain += "t = g" + std::to_string(k - 1) + "(x);\n  return t;\n}\n";
  }
  CHECK(fails_with(chain + "vec4 main(vec2 p) { float t = g12(p.x); return vec4(t); }",
                   "nesting is deeper than 512 after inlining"));
}

TEST_CASE("shader lang: statement and name rules")
{
  // A function cannot call itself (it is registered before its body).
  CHECK(fails_at("float f(float x) { float t = f(x); return t; }\n" + with_main("  return vec4(f(p.x));"),
                 1, 30, "'f' calls itself; recursion is not supported"));
  CHECK(fails_with("float f(int n, float x) { float t = x; if (n > 0) { t = f(n - 1, x * 2.0); } return t; }\n" +
                       with_main("  return vec4(f(3, p.x));"),
                   "recursion is not supported"));
  // A statement that starts with a call must be just the call.
  CHECK(fails_at(with_main("  abs(p.x) == 1.0;\n  return vec4(0.0);"), 2, 3, "expected a function call statement"));
  CHECK(fails_at("float f(float x) { return x; }\n" + with_main("  f(p.x) + 1.0;\n  return vec4(0.0);"), 3, 3,
                 "expected a function call statement"));
  CHECK(compiles("void g(out float y) { y = 1.0; }\n" + with_main("  float y;\n  g(y);\n  return vec4(y);")));
  // A local cannot take a function's name.
  CHECK(fails_at("float f(float x) { return x; }\n" + with_main("  float f = 2.0;\n  return vec4(f);"), 3, 9,
                 "'f' is a function"));
  // A long loop that does end says how often it runs.
  CHECK(fails_at(with_main("  for (int i = 0; i < 2000000; i++) {}\n  return vec4(0.0);"), 2, 3,
                 "loop runs 2000000 times; the limit is 64"));
  CHECK(fails_at(with_main("  for (int i = 100; i >= 0; i -= 1) {}\n  return vec4(0.0);"), 2, 3,
                 "loop runs 101 times; the limit is 64"));
  CHECK(fails_at(with_main("  for (int i = 0; i != 200; i += 2) {}\n  return vec4(0.0);"), 2, 3,
                 "loop runs 100 times; the limit is 64"));
}

TEST_CASE("shader lang: float literals round correctly, whatever the C locale")
{
  struct Lit { const char* text; float expect; };
  const Lit lits[] = {
    {"0.1", 0.1f}, {"0.30000001192092896", 0.3f}, {"3.4028235e38", 3.4028235e38f},
    {"1.17549435e-38", 1.17549435e-38f}, {"1e-45", 1e-45f}, {"7e-46", 0.0f},
    {"16777217.0", 16777216.0f},      // tie, rounds to even
    {"16777219.0", 16777220.0f},      // tie, rounds to even
    {"1.00000005960464477539", 1.0f},  // exactly halfway between 1 and the next float
    {"1.0000000596046448", 1.0000001192092896f},  // just above halfway
    {"123456789012345678901234567890.0", 1.2345679e29f}, {".5", 0.5f}, {"2.", 2.0f}, {"1e2", 100.0f},
    {"2.5E-3", 0.0025f}, {"0.0", 0.0f}};
  const char* const old = std::setlocale(LC_NUMERIC, nullptr);
  const std::string saved = old ? old : "C";
  // A decimal-comma locale, when the system has one, must not change anything.
  if (!std::setlocale(LC_NUMERIC, "de_DE.UTF-8")) std::setlocale(LC_NUMERIC, "de_DE");
  for (const Lit& l : lits) {
    const std::string src = std::string("uniform float u = ") + l.text + ";\n" + with_main("return vec4(u);");
    const nsl::CompileResult r = compile(src);
    REQUIRE(r.program != nullptr);
    const float got = nsl::uniforms(*r.program)[0].defaults[0];
    uint32_t a, b;
    std::memcpy(&a, &got, 4);
    std::memcpy(&b, &l.expect, 4);
    if (a != b) std::fprintf(stderr, "  literal %s -> %.9g, expected %.9g\n", l.text, static_cast<double>(got), static_cast<double>(l.expect));
    CHECK_EQ(a, b);
  }
  std::setlocale(LC_NUMERIC, saved.c_str());
}

TEST_CASE("shader lang: pragmas, flags and determinism")
{
  auto flags_of = [](const std::string& src) {
    const nsl::CompileResult r = compile(src);
    return r.program ? nsl::flags(*r.program) : 0xffffffffu;
  };
  CHECK_EQ(flags_of(with_main("return vec4(0.0);")), 0u);
  CHECK_EQ(flags_of("#pragma neui dither\n" + with_main("return vec4(0.0);")), static_cast<uint32_t>(nsl::NSL_DITHER));
  CHECK_EQ(flags_of("  #pragma neui cpu_only // trailing comment\n" + with_main("return vec4(0.0);")),
           static_cast<uint32_t>(nsl::NSL_CPU_ONLY));
  CHECK_EQ(flags_of(with_main("return vec4(neui_time);")), static_cast<uint32_t>(nsl::NSL_READS_TIME));
  // The same source always compiles to the same program.
  for (const char* src : k_corpus) {
    const nsl::CompileResult a = compile(src), b = compile(src);
    REQUIRE(a.program != nullptr);
    REQUIRE(b.program != nullptr);
    CHECK_EQ(nsl::stats(*a.program).pixel_ops, nsl::stats(*b.program).pixel_ops);
    CHECK_EQ(nsl::stats(*a.program).draw_ops, nsl::stats(*b.program).draw_ops);
    CHECK_EQ(nsl::stats(*a.program).pixel_regs, nsl::stats(*b.program).pixel_regs);
  }
}

TEST_CASE("shader lang: mutated programs never crash the compiler")
{
  static const char k_chars[] = "(){};,.+-*/<>=!&|^?:#0123456789eExyzpvf \n";
  uint32_t seed = 2026u;
  auto rnd = [&](uint32_t n) {
    seed = seed * 1664525u + 1013904223u;
    return n ? (seed >> 8) % n : 0u;
  };
  int compiled = 0, failed = 0;
  for (const char* base : k_corpus) {
    const std::string src = base;
    // Every prefix (truncated files).
    for (size_t cut = 0; cut < src.size(); cut += 7) {
      const nsl::CompileResult r = compile(src.substr(0, cut));
      if (r.program) ++compiled;
      else { ++failed; CHECK(r.line >= 1 && r.col >= 1); }
    }
    // Random edits.
    for (int m = 0; m < 600; ++m) {
      std::string s = src;
      const int edits = 1 + static_cast<int>(rnd(3));
      for (int e = 0; e < edits && !s.empty(); ++e) {
        const size_t at = rnd(static_cast<uint32_t>(s.size()));
        switch (rnd(4)) {
          case 0: s.erase(at, 1 + rnd(3)); break;
          case 1: s.insert(at, 1, k_chars[rnd(sizeof(k_chars) - 1)]); break;
          case 2: s[at] = k_chars[rnd(sizeof(k_chars) - 1)]; break;
          default: s.insert(at, s.substr(rnd(static_cast<uint32_t>(s.size())), 1 + rnd(8))); break;
        }
      }
      const nsl::CompileResult r = compile(s);
      if (r.program) ++compiled;
      else { ++failed; CHECK(r.line >= 1 && r.col >= 1); }
    }
  }
  CHECK(compiled > 0);
  CHECK(failed > 0);
}
