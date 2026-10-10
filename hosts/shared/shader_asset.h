#pragma once

// SHADER assets (NEUI_ASSET_KIND_SHADER): what an asset slot holds for a
// compiled .nsl program, the bake logic behind NEUI_API_SHADER's
// bake_bitmap / bake_filmstrip, and the cache of rendered images behind the
// live paths (the compound SHADER layer and draw, shader_layer.h). Host-free,
// so the Tier-1 suite covers it.
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
#include <unordered_map>
#include <vector>

#include <neui/d/renderer.h>
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

  // ---- Cache of rendered images ---------------------------------------------
  //
  // The live paths render a shader into a bitmap and draw that, re-running the
  // shader only when an input changes. One cache per session (on the asset
  // store), keyed by content: the program, the layer size, the scale, the
  // sub-pixel position of the layer on the device grid, the uniform values and
  // the time. So fifty knobs at one value share one image, and a binding's
  // ".step" quantisation bounds the images automation can produce. Least
  // recently used images go once the CPU pixels exceed a byte budget; each
  // keeps one upload per render context (re-made after device loss).

  // The inputs an image is rendered from. `time` is 0 for a program that
  // does not read neui_time; `uniforms` holds uniform_components floats.
  struct ShaderCacheKey
  {
    uint64_t     uid = 0;
    float        w = 0.0f, h = 0.0f;  // layer size, logical px
    float        scale = 1.0f;        // shader px per logical px
    float        fx = 0.0f, fy = 0.0f;  // origin minus its floor, shader px
    float        time = 0.0f;
    const float* uniforms = nullptr;
    size_t       uniform_count = 0;
  };

  struct ShaderCacheEntry
  {
    struct Upload { void* bmp = nullptr; uint32_t generation = 0; };

    // The key, owned.
    uint64_t           uid = 0;
    float              w = 0.0f, h = 0.0f, scale = 1.0f, fx = 0.0f, fy = 0.0f, time = 0.0f;
    std::vector<float> uniforms;
    uint64_t           hash = 0;

    // The image: BGRA8 premultiplied, w_px x h_px, belonging (px, py) shader
    // px from the floored origin (always 0 in practice).
    std::vector<uint8_t> pixels;
    uint32_t             w_px = 0, h_px = 0;
    int                  px = 0, py = 0;
    std::unordered_map<neui_render_ctx_t, Upload> uploads;
    uint64_t             stamp = 0;  // last use (LRU)
  };

  class ShaderCache
  {
  public:
    static constexpr size_t k_default_budget = size_t(48) << 20;

    static uint64_t hash_key(const ShaderCacheKey& k)
    {
      uint64_t h = 1469598103934665603ull;  // FNV-1a
      auto mix = [&h](const void* p, size_t n) {
        const auto* b = static_cast<const uint8_t*>(p);
        for (size_t i = 0; i < n; ++i) { h ^= b[i]; h *= 1099511628211ull; }
      };
      mix(&k.uid, sizeof k.uid);
      const float f[6] = { k.w, k.h, k.scale, k.fx, k.fy, k.time };
      mix(f, sizeof f);
      if (k.uniform_count) mix(k.uniforms, sizeof(float) * k.uniform_count);
      return h;
    }

    // The image for `key`, rendered by render(entry) on a miss (it fills
    // pixels, w_px, h_px, px, py and returns false on failure, which caches
    // nothing). Null on failure. Evicts least recently used images past the
    // budget, never the one returned.
    template <typename RenderFn>
    ShaderCacheEntry* get(const ShaderCacheKey& key, RenderFn&& render,
                          neui_render_backend_t* backend)
    {
      const uint64_t h = hash_key(key);
      auto range = _map.equal_range(h);
      for (auto it = range.first; it != range.second; ++it) {
        ShaderCacheEntry& e = *it->second;
        if (matches(e, key)) { e.stamp = ++_clock; ++_hits; return &e; }
      }
      auto e = std::make_unique<ShaderCacheEntry>();
      e->uid = key.uid;
      e->w = key.w; e->h = key.h; e->scale = key.scale;
      e->fx = key.fx; e->fy = key.fy; e->time = key.time;
      e->uniforms.assign(key.uniforms, key.uniforms + key.uniform_count);
      e->hash = h;
      try {
        if (!render(*e)) return nullptr;
      } catch (const std::bad_alloc&) {
        return nullptr;
      }
      ++_renders;
      e->stamp = ++_clock;
      _bytes += e->pixels.size();
      ShaderCacheEntry* out = e.get();
      _map.emplace(h, std::move(e));
      evict_to_budget(out, backend);
      return out;
    }

    // The entry's bitmap for `ctx`, uploaded on first use and again after a
    // generation bump (device loss). Null when the backend cannot make one.
    static void* upload(ShaderCacheEntry& e, neui_render_backend_t* backend,
                        neui_render_ctx_t ctx)
    {
      if (!backend || !ctx || e.pixels.empty()) return nullptr;
      const uint32_t gen = backend->get_context_generation
        ? backend->get_context_generation(ctx) : 0u;
      auto it = e.uploads.find(ctx);
      if (it != e.uploads.end() && it->second.generation != gen) {
        if (backend->destroy_bitmap && it->second.bmp)
          backend->destroy_bitmap(ctx, it->second.bmp);
        e.uploads.erase(it);
        it = e.uploads.end();
      }
      if (it == e.uploads.end()) {
        if (!backend->create_bitmap) return nullptr;
        void* bmp = backend->create_bitmap(ctx, e.w_px, e.h_px, e.pixels.data(), e.scale);
        if (!bmp) return nullptr;
        it = e.uploads.emplace(ctx, ShaderCacheEntry::Upload{ bmp, gen }).first;
      }
      return it->second.bmp;
    }

    // Every image of a program, when its SHADER asset goes.
    void release_uid(uint64_t uid, neui_render_backend_t* backend)
    {
      for (auto it = _map.begin(); it != _map.end();) {
        if (it->second->uid == uid) it = erase(it, backend);
        else ++it;
      }
    }

    // A render context is dying: its uploads go, the images stay.
    void release_ctx(neui_render_ctx_t ctx, neui_render_backend_t* backend)
    {
      for (auto& kv : _map) {
        auto u = kv.second->uploads.find(ctx);
        if (u == kv.second->uploads.end()) continue;
        if (backend && backend->destroy_bitmap && u->second.bmp)
          backend->destroy_bitmap(ctx, u->second.bmp);
        kv.second->uploads.erase(u);
      }
    }

    void clear(neui_render_backend_t* backend)
    {
      for (auto& kv : _map) destroy_uploads(*kv.second, backend);
      _map.clear();
      _bytes = 0;
    }

    void set_budget(size_t bytes, neui_render_backend_t* backend)
    {
      _budget = bytes;
      evict_to_budget(nullptr, backend);
    }

    size_t   budget() const  { return _budget; }
    size_t   bytes() const   { return _bytes; }
    size_t   size() const    { return _map.size(); }
    uint64_t renders() const { return _renders; }  // misses that rendered
    uint64_t hits() const    { return _hits; }

  private:
    using Map = std::unordered_multimap<uint64_t, std::unique_ptr<ShaderCacheEntry>>;

    static bool matches(const ShaderCacheEntry& e, const ShaderCacheKey& k)
    {
      auto same = [](float a, float b) { return std::memcmp(&a, &b, sizeof a) == 0; };
      return e.uid == k.uid && same(e.w, k.w) && same(e.h, k.h) && same(e.scale, k.scale) &&
             same(e.fx, k.fx) && same(e.fy, k.fy) && same(e.time, k.time) &&
             e.uniforms.size() == k.uniform_count &&
             (k.uniform_count == 0 ||
              std::memcmp(e.uniforms.data(), k.uniforms, sizeof(float) * k.uniform_count) == 0);
    }

    static void destroy_uploads(ShaderCacheEntry& e, neui_render_backend_t* backend)
    {
      if (backend && backend->destroy_bitmap)
        for (auto& [ctx, u] : e.uploads)
          if (u.bmp) backend->destroy_bitmap(ctx, u.bmp);
      e.uploads.clear();
    }

    Map::iterator erase(Map::iterator it, neui_render_backend_t* backend)
    {
      destroy_uploads(*it->second, backend);
      _bytes -= it->second->pixels.size();
      return _map.erase(it);
    }

    void evict_to_budget(const ShaderCacheEntry* keep, neui_render_backend_t* backend)
    {
      while (_bytes > _budget) {
        auto victim = _map.end();
        for (auto it = _map.begin(); it != _map.end(); ++it)
          if (it->second.get() != keep &&
              (victim == _map.end() || it->second->stamp < victim->second->stamp))
            victim = it;
        if (victim == _map.end()) return;
        erase(victim, backend);
      }
    }

    Map      _map;
    size_t   _bytes   = 0;
    size_t   _budget  = k_default_budget;
    uint64_t _clock   = 0;
    uint64_t _renders = 0;
    uint64_t _hits    = 0;
  };

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
