#pragma once

// SHADER assets (NEUI_ASSET_KIND_SHADER): what an asset slot holds for a
// compiled .nsl program, and the bake logic behind NEUI_API_SHADER's
// bake_bitmap / bake_filmstrip. Host-free, so the Tier-1 suite covers it.
//
// The shader arithmetic itself lives in the neui-shader library
// (src/shader/nsl.h); nothing here computes a pixel. That matters: the
// library is built with its own flags so every platform gets the same bits,
// and this header is compiled with whatever flags its includer uses.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <memory>
#include <new>
#include <vector>

#include <neui/d/shader.h>

#include "../../src/shader/nsl.h"

namespace neui_detail
{
  struct ShaderAsset
  {
    std::shared_ptr<const neui::nsl::Program> program;
    // Process-unique and never reused (a slot is), so a cache can key on it.
    uint64_t uid = 0;
  };

  inline uint64_t next_shader_uid()
  {
    static uint64_t n = 0;
    return ++n;
  }

  // The uniform value array execute() takes: every declared default, then
  // the named values over it. Names the program does not declare are ignored.
  inline void shader_uniform_values(const neui::nsl::Program& prog,
                                    const neui_shader_uniform_t* values,
                                    uint32_t count, std::vector<float>& out)
  {
    out.assign(static_cast<size_t>(neui::nsl::uniform_components(prog)), 0.0f);
    if (!out.empty()) neui::nsl::default_uniforms(prog, out.data());
    if (!values) return;
    const auto& decls = neui::nsl::uniforms(prog);
    for (uint32_t i = 0; i < count; ++i) {
      if (!values[i].name) continue;
      for (const auto& u : decls) {
        if (u.name != values[i].name) continue;
        std::memcpy(&out[static_cast<size_t>(u.offset)], values[i].v,
                    sizeof(float) * static_cast<size_t>(u.components));
        break;
      }
    }
  }

  // The cell grid for a strip of `frames` cells of w x h px: the column count
  // that keeps the longer bitmap side shortest, fewer columns on a tie. GPU
  // texture limits apply per side, and a long single column (the audio-plugin
  // convention) exceeds them quickly: 128 frames at 240 px are 30720 px tall.
  // Frames are drawn by index, so the layout is invisible to clients. False
  // when no grid fits max_side px a side.
  inline bool shader_strip_grid(uint32_t frames, uint32_t w, uint32_t h,
                                uint32_t max_side, uint32_t& cols, uint32_t& rows)
  {
    if (frames == 0 || w == 0 || h == 0) return false;
    uint64_t best = UINT64_MAX;
    const uint32_t max_cols = std::min<uint32_t>(frames, max_side / w);
    for (uint32_t c = 1; c <= max_cols; ++c) {
      const uint32_t r = (frames + c - 1) / c;
      const uint64_t side_w = static_cast<uint64_t>(c) * w;
      const uint64_t side_h = static_cast<uint64_t>(r) * h;
      if (side_h > max_side) continue;
      const uint64_t longer = std::max(side_w, side_h);
      if (longer < best) { best = longer; cols = c; rows = r; }
    }
    return best != UINT64_MAX;
  }

  struct ShaderBakeResult
  {
    std::vector<uint8_t> pixels;  // BGRA8 premultiplied, w_px * h_px * 4
    uint32_t w_px = 0, h_px = 0;
    uint32_t cols = 1, rows = 1, frames = 1;
    float    scale = 1.0f;
  };

  // Renders `frames` cells (1 for a plain bake). With `sweep` set, cell i
  // sets that uniform (or neui_time) to from + (to - from) * i / (frames - 1).
  // False for a bad bake, a sweep that is not a float uniform or neui_time,
  // a size over the limits, or an allocation failure.
  inline bool shader_bake(const ShaderAsset& sa, const neui_shader_bake_t& b,
                          const char* sweep, float from, float to, uint32_t frames,
                          ShaderBakeResult& out)
  {
    namespace nsl = neui::nsl;
    if (!sa.program || frames == 0) return false;
    const nsl::Program& prog = *sa.program;

    const float scale = b.scale > 0.0f ? b.scale : 1.0f;
    if (!std::isfinite(b.width) || !std::isfinite(b.height) || !std::isfinite(scale) ||
        !(b.width > 0.0f) || !(b.height > 0.0f))
      return false;

    nsl::Draw d;
    d.w = b.width;
    d.h = b.height;
    d.scale = scale;
    d.time = nsl::wrap_time(b.time);
    const nsl::Placement cell = nsl::placement(d);
    if (cell.w <= 0 || cell.h <= 0) return false;

    std::vector<float> values;
    shader_uniform_values(prog, b.uniforms, b.uniform_count, values);
    d.uniforms = values.empty() ? nullptr : values.data();

    // What the sweep writes: a float uniform's slot, or the time.
    float* swept = nullptr;
    bool sweep_time = false;
    if (sweep) {
      if (std::strcmp(sweep, "neui_time") == 0) {
        sweep_time = true;
      } else {
        for (const auto& u : nsl::uniforms(prog))
          if (u.name == sweep && u.components == 1) swept = &values[static_cast<size_t>(u.offset)];
        if (!swept) return false;
      }
    } else if (frames != 1) {
      return false;
    }

    uint32_t cols = 1, rows = 1;
    if (!shader_strip_grid(frames, static_cast<uint32_t>(cell.w), static_cast<uint32_t>(cell.h),
                           static_cast<uint32_t>(nsl::k_max_bitmap_side), cols, rows))
      return false;
    const uint32_t w_px = cols * static_cast<uint32_t>(cell.w);
    const uint32_t h_px = rows * static_cast<uint32_t>(cell.h);
    const size_t stride = static_cast<size_t>(w_px) * 4u;

    try {
      out.pixels.assign(stride * h_px, 0);
    } catch (const std::bad_alloc&) {
      return false;
    }
    for (uint32_t i = 0; i < frames; ++i) {
      if (sweep) {
        const float v = nsl::sweep_value(from, to, i, frames);
        if (sweep_time) d.time = nsl::wrap_time(v);
        else *swept = v;
      }
      const size_t ox = static_cast<size_t>(i % cols) * static_cast<size_t>(cell.w);
      const size_t oy = static_cast<size_t>(i / cols) * static_cast<size_t>(cell.h);
      nsl::execute(prog, d, out.pixels.data() + oy * stride + ox * 4u, stride);
    }
    out.w_px = w_px;
    out.h_px = h_px;
    out.cols = cols;
    out.rows = rows;
    out.frames = frames;
    out.scale = scale;
    return true;
  }
}  // namespace neui_detail
