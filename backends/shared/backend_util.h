#pragma once

#include <cstdint>
#include <vector>

// Backend-agnostic helpers shared by the d2d and cg backends. Each backend
// keeps its platform-idiomatic state (D2D's explicit transform stack vs
// CG's SaveGState, struct-keyed vs string-keyed font caches) - only the
// pure math that was previously copy-pasted lives here.

namespace neui_detail
{
  // Unpack 0xAARRGGBB into normalised [0..1] RGBA channels, with an
  // optional extra alpha multiplier (the backends fold the alpha stack in
  // here). Templated on the channel type so CG's CGFloat (double) and
  // D2D's float both bind without narrowing.
  template <typename F>
  inline void argb_unpack(uint32_t argb, F out[4], float alpha_mul = 1.0f)
  {
    out[0] = static_cast<F>(static_cast<float>((argb >> 16) & 0xFF) / 255.0f);              // R
    out[1] = static_cast<F>(static_cast<float>((argb >>  8) & 0xFF) / 255.0f);              // G
    out[2] = static_cast<F>(static_cast<float>( argb        & 0xFF) / 255.0f);              // B
    out[3] = static_cast<F>(static_cast<float>((argb >> 24) & 0xFF) / 255.0f * alpha_mul);  // A
  }

  // Elevate a quadratic Bézier (current point + one control + end) to the
  // equivalent cubic (two controls + end), for backends with no native quad
  // (Cairo). CP1 = cur + 2/3 (ctrl - cur); CP2 = end + 2/3 (ctrl - end). The
  // resulting cubic is geometrically identical to the quadratic. Outputs the
  // two cubic control points; the end point is unchanged.
  template <typename F>
  inline void quad_to_cubic(F cur_x, F cur_y, F ctrl_x, F ctrl_y, F end_x, F end_y,
                            F out_c1[2], F out_c2[2])
  {
    out_c1[0] = cur_x + static_cast<F>(2.0 / 3.0) * (ctrl_x - cur_x);
    out_c1[1] = cur_y + static_cast<F>(2.0 / 3.0) * (ctrl_y - cur_y);
    out_c2[0] = end_x + static_cast<F>(2.0 / 3.0) * (ctrl_x - end_x);
    out_c2[1] = end_y + static_cast<F>(2.0 / 3.0) * (ctrl_y - end_y);
  }

  // Cumulative-opacity stack semantics (renderer.h push_alpha/pop_alpha):
  // each push multiplies onto the previous top; empty = fully opaque.
  // Clamping happens at push so a single bad factor can't poison the
  // products further up the stack.
  inline void alpha_stack_push(std::vector<float>& stack, float factor)
  {
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    float prev = stack.empty() ? 1.0f : stack.back();
    stack.push_back(prev * factor);
  }

  inline void alpha_stack_pop(std::vector<float>& stack)
  {
    if (!stack.empty()) stack.pop_back();
  }

  inline float alpha_stack_current(const std::vector<float>& stack)
  {
    return stack.empty() ? 1.0f : stack.back();
  }

  // Font-cache size quantisation: 0.1-logical-pixel buckets so
  // floating-point chatter (12.0 vs 12.00001) doesn't churn cache
  // entries. Both backends key their font caches on this value.
  inline uint32_t font_size_q10(float font_size)
  {
    return static_cast<uint32_t>(font_size * 10.0f + 0.5f);
  }

} // namespace neui_detail
