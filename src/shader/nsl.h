// neui shading language (.nsl): compiler + CPU executor.
//
// The language is specified in docs/shader-language.md; the design and its
// rationale in plans/shader-effects.md. This header is the internal C++ API
// the hosts build NEUI_API_SHADER on. Everything behind it lives in the
// neui-shader library (src/shader/), the only place shader arithmetic is
// compiled, always optimised and never FMA-contracted, so every platform
// produces the same bits.

#pragma once

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

namespace neui {
namespace nsl {

struct Program;  // compiled and immutable; share it freely

struct CompileResult {
  std::shared_ptr<const Program> program;  // null on error
  std::string error;                       // "<name>:<line>:<col>: error: <message>"
  int line = 0, col = 0;                   // of the error, 1-based
};

// Compiles .nsl source. `name` appears in the diagnostic ("<source>" if null).
CompileResult compile(const char* src, size_t len, const char* name = nullptr);

// A uniform as declared: `components` floats starting at `offset` in the
// uniform value array that execute() takes.
struct UniformDesc {
  std::string name;
  int components = 1;
  int offset = 0;
  float defaults[4] = {0.0f, 0.0f, 0.0f, 0.0f};
};

const std::vector<UniformDesc>& uniforms(const Program& p);
int uniform_components(const Program& p);  // total floats in the value array
// Writes every uniform's default into `values` (uniform_components floats).
void default_uniforms(const Program& p, float* values);

enum : uint32_t {
  NSL_READS_TIME = 1u << 0,  // reads neui_time: animates
  NSL_DITHER     = 1u << 1,  // #pragma neui dither
  NSL_CPU_ONLY   = 1u << 2,  // #pragma neui cpu_only
};
uint32_t flags(const Program& p);

struct Stats {
  int pixel_ops = 0;        // scalar operations per pixel
  int draw_ops = 0;         // operations evaluated once per draw (uniform-only work)
  int pixel_regs = 0;       // live scalar registers per pixel
  int transcendentals = 0;  // per-pixel sin / cos / exp2 / log2 / atan2
};
Stats stats(const Program& p);

// neui_time for a clock reading in seconds: wrapped to [0, 3600) as the
// spec requires (negative readings wrap too; NaN and infinities give 0).
float wrap_time(double seconds);

// Value i of n evenly spaced from `from` to `to`, both ends exact (a
// filmstrip sweep). Here and not in the caller so no compiler contracts it.
float sweep_value(float from, float to, uint32_t i, uint32_t n);

// One draw of a shader layer.
struct Draw {
  float x = 0.0f, y = 0.0f;   // layer origin, logical px, translation included
  float w = 0.0f, h = 0.0f;   // layer size, logical px
  float scale = 1.0f;         // device px per logical px (display scale x resolution)
  float time = 0.0f;          // neui_time, already wrapped to [0, 3600)
  const float* uniforms = nullptr;  // uniform_components floats; null = defaults
};

// The device pixels a draw covers: the bitmap is w x h and belongs at device
// position (x, y). Empty (w or h 0) when the layer is empty or too large.
struct Placement {
  int x = 0, y = 0, w = 0, h = 0;
};
Placement placement(const Draw& d);

// Largest bitmap side execute() accepts.
constexpr int k_max_bitmap_side = 16384;

// Runs the shader over every pixel of placement(d) and writes premultiplied
// BGRA8 rows of `stride` bytes into `bgra`. Returns false (and writes
// nothing) when the placement is empty.
bool execute(const Program& p, const Draw& d, uint8_t* bgra, size_t stride);

// ---- GPU executors -----------------------------------------------------------
//
// emit_hlsl() writes the pixel program, neui's math and the output rule as a
// ps_4_0 pixel shader for a zero-input Direct2D custom effect: entry `main`
// with SV_POSITION and SCENE_POSITION inputs, constant buffer b0. The effect
// is drawn over the device pixels placement() names, with SCENE_POSITION
// starting at 0 at their top-left. It outputs premultiplied colour already
// rounded to 8 bits as the CPU does (coverage, dithering included), times an
// opacity (k_gpu_opacity). Empty when the program must stay on the CPU
// (#pragma neui cpu_only, or more constants than a constant buffer holds).
std::string emit_hlsl(const Program& p);

// Floats in the constant buffer of one draw (a multiple of 4; 0 when
// emit_hlsl is empty).
size_t gpu_constant_count(const Program& p);

// Fills the constant buffer for draw `d`: the pixel grid of placement(d) and
// the values the pixel program loads, the draw program run here on the CPU.
// False (nothing written) when placement(d) is empty or the program has no
// GPU form.
bool gpu_constants(const Program& p, const Draw& d, float* out);

// The constant a backend multiplies its opacity (alpha stack) into: 1 as
// gpu_constants writes it.
constexpr size_t k_gpu_opacity = 6;

// Test hook: the same draw through scalar kernels, one pixel at a time. Must
// produce exactly the bytes execute() does.
bool execute_scalar(const Program& p, const Draw& d, uint8_t* bgra, size_t stride);

// Test hook: evaluates main at n positions (xs[i], ys[i]) for draw d and
// writes its raw return values, 4 floats per position, before any clamping.
// `simd` selects the 64-lane SIMD kernels or the scalar ones.
void evaluate(const Program& p, const Draw& d, const float* xs, const float* ys, int n,
              float* out, bool simd);

}  // namespace nsl
}  // namespace neui
