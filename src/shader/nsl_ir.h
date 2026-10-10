// .nsl intermediate representation, shared by the compiler (nsl_compile.cpp)
// and the executor (nsl_exec.cpp). Private to src/shader/.
//
// A compiled program is scalar three-address code in two parts:
//   - the draw program: work that depends only on constants, uniforms and the
//     reserved uniforms, evaluated once per draw with scalar math;
//   - the pixel program: work that depends on the pixel position, evaluated
//     for 64 pixels at a time with SIMD kernels.
// Booleans are floats holding 0 or 1.

#pragma once

#include "nsl.h"
#include "nsl_math.h"

#include <utility>
#include <vector>

namespace neui {
namespace nsl {
namespace detail {

enum Op : uint8_t {
  OP_ADD, OP_SUB, OP_MUL, OP_DIV, OP_NEG, OP_MIN, OP_MAX, OP_ABS, OP_FLOOR, OP_SQRT,
  OP_SIN, OP_COS, OP_EXP2, OP_LOG2, OP_ATAN2, OP_HASH2,
  OP_LT, OP_LE, OP_EQ, OP_NE,  // 1 when the comparison holds, else 0
  OP_SEL,                      // a != 0 ? b : c
  OP_LOAD,                     // pixel program only: d = pixel_pool[a], broadcast
  OP_COUNT
};

inline int op_arity(Op op) {
  switch (op) {
    case OP_NEG: case OP_ABS: case OP_FLOOR: case OP_SQRT:
    case OP_SIN: case OP_COS: case OP_EXP2: case OP_LOG2: case OP_LOAD:
      return 1;
    case OP_SEL:
      return 3;
    default:
      return 2;
  }
}

inline bool op_is_transcendental(Op op) {
  return op == OP_SIN || op == OP_COS || op == OP_EXP2 || op == OP_LOG2 || op == OP_ATAN2;
}

// The scalar definition of every op. The compiler folds constants with it and
// the executor runs the draw program with it, so folding, draw-time and
// pixel-time evaluation agree bit for bit.
inline float eval_op(Op op, float a, float b, float c) {
  switch (op) {
    case OP_ADD:   return a + b;
    case OP_SUB:   return a - b;
    case OP_MUL:   return a * b;
    case OP_DIV:   return a / b;
    case OP_NEG:   return from_bits(bits(a) ^ 0x80000000u);
    case OP_MIN:   return m_min(a, b);
    case OP_MAX:   return m_max(a, b);
    case OP_ABS:   return m_abs(a);
    case OP_FLOOR: return m_floor(a);
    case OP_SQRT:  return std::sqrt(a);
    case OP_SIN:   return m_sin(a);
    case OP_COS:   return m_cos(a);
    case OP_EXP2:  return m_exp2(a);
    case OP_LOG2:  return m_log2(a);
    case OP_ATAN2: return m_atan2(a, b);
    case OP_HASH2: return m_hash2(a, b);
    case OP_LT:    return a < b ? 1.0f : 0.0f;
    case OP_LE:    return a <= b ? 1.0f : 0.0f;
    case OP_EQ:    return a == b ? 1.0f : 0.0f;
    case OP_NE:    return a != b ? 1.0f : 0.0f;
    case OP_SEL:   return a != 0.0f ? b : c;
    default:       return 0.0f;
  }
}

struct Ins {
  Op op;
  uint16_t d, a, b, c;  // registers; unused operands repeat a
};

// The reserved uniforms, as draw-program inputs.
enum Reserved : uint8_t { RES_SIZE_X, RES_SIZE_Y, RES_SCALE, RES_PX, RES_TIME, RES_COUNT };

template <typename T>
using RegInit = std::pair<uint16_t, T>;  // register <- value / source

}  // namespace detail

struct Program {
  // Draw program (scalar, once per draw).
  std::vector<detail::Ins> draw_code;
  int draw_regs = 0;
  std::vector<detail::RegInit<float>> draw_consts;
  std::vector<detail::RegInit<uint16_t>> draw_uniforms;  // <- uniform value index
  std::vector<detail::RegInit<uint8_t>> draw_reserved;   // <- Reserved

  // Pixel program (SIMD, 64 pixels at a time). Constants and draw values
  // live in a pool and reach registers through OP_LOAD: normally all of them
  // in a prologue that runs once per draw (pixel_prologue instructions); when
  // that would need too many registers, each right before its first use.
  struct PoolEntry {
    float value = 0.0f;
    int draw_reg = -1;  // >= 0: the value is that draw-program register
  };
  std::vector<detail::Ins> pixel_code;
  int pixel_prologue = 0;
  int pixel_regs = 0;
  std::vector<PoolEntry> pixel_pool;
  uint16_t px = 0, py = 0;         // the pixel position
  uint16_t out[4] = {0, 0, 0, 0};  // straight r, g, b, a

  std::vector<UniformDesc> uniform_list;
  int uniform_count = 0;  // floats
  uint32_t flag_bits = 0;
  Stats stat;
};

namespace detail {

// The GPU constant buffer (nsl_gpu.cpp writes the shader that reads it,
// nsl_exec.cpp fills it): two float4 of pixel grid, then the draw-program
// values the pixel program loads, one float each, in pixel_pool order:
//   c[0] = (fx, fy, X1, Y1)   origin remainder and far edges, relative to
//                             the floored origin (device px)
//   c[1] = (scale, 0, opacity, 0)
constexpr size_t k_gpu_header = 8;
constexpr size_t k_gpu_max_floats = 4096 * 4;  // one D3D constant buffer

// For each pixel_pool entry, its float index in the constant buffer, or -1
// for a constant (emitted into the shader text).
inline std::vector<int> gpu_pool_slots(const Program& p) {
  std::vector<int> slot(p.pixel_pool.size(), -1);
  size_t n = k_gpu_header;
  for (size_t i = 0; i < slot.size(); ++i)
    if (p.pixel_pool[i].draw_reg >= 0) slot[i] = static_cast<int>(n++);
  return slot;
}

inline size_t gpu_float_count(const Program& p) {
  size_t n = k_gpu_header;
  for (const auto& e : p.pixel_pool)
    if (e.draw_reg >= 0) ++n;
  return (n + 3) / 4 * 4;
}

}  // namespace detail
}  // namespace nsl
}  // namespace neui
