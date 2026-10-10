#pragma once

// The live shader paths: the compound SHADER layer (NEUI_COMPOUND_LAYER_SHADER)
// and NEUI_API_SHADER's draw. Both resolve the shader through the painter's
// resolve_shader thunk, render it on the CPU into the session's image cache
// (ShaderCache, shader_asset.h) and draw the image onto whole device pixels,
// so it is never resampled. Host-free, so the Tier-1 suite covers it with a
// fake backend.

#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

#include <neui/d/shader.h>

#include "attrs.h"
#include "compound.h"
#include "painter.h"
#include "shader_asset.h"

namespace neui_detail
{
  // A layer's or draw's "resolution": (0, 4], anything else 1. Below 1/16 it
  // is clamped to 1/16, which is already a 16x upscale.
  inline float shader_resolution_clamp(float r)
  {
    if (!(r > 0.0f)) return 1.0f;  // NaN too
    if (r < 0.0625f) return 0.0625f;
    return r < 4.0f ? r : 4.0f;
  }

  // Where the shader's pixels lie: shader px U = k * x + ox for a logical x
  // of the painter's current space, and likewise y. When the current
  // transform is a translation plus a uniform positive scale, this is the
  // device grid times the resolution, so an image placed on whole shader px
  // lands on whole device px. Otherwise (rotation, skew, mirror, uneven
  // scale) the image is shaded in the painter's space at the transform's
  // average scale and drawn through the transform.
  struct ShaderGrid
  {
    float k = 1.0f, ox = 0.0f, oy = 0.0f;
    bool  device = false;  // shader px are device px (aligned, resolution 1)
  };

  inline ShaderGrid shader_grid(neui_painter_t* p, float resolution)
  {
    float m[6] = { 1.0f, 0.0f, 0.0f, 1.0f, 0.0f, 0.0f };
    if (p->backend->get_device_transform) {
      p->backend->get_device_transform(p->ctx, m);
    } else {
      const float s = painter_get_scale_factor(p);
      m[0] = m[3] = s > 0.0f ? s : 1.0f;
    }
    ShaderGrid g;
    const float tol = 1e-5f * std::fabs(m[0]);
    if (m[0] > 0.0f && std::fabs(m[1]) <= tol && std::fabs(m[2]) <= tol &&
        std::fabs(m[3] - m[0]) <= tol) {
      g.k  = m[0] * resolution;
      g.ox = m[4] * resolution;
      g.oy = m[5] * resolution;
      g.device = resolution == 1.0f;
    } else {
      g.k = std::sqrt(std::fabs(m[0] * m[3] - m[1] * m[2])) * resolution;
    }
    return g;
  }

  // Draws `sa` over (x, y, w, h) of the painter's current space with the
  // given uniform values (uniform_components floats; null = the defaults)
  // and neui_time `time` (already wrapped). False when nothing was drawn.
  //
  // On the device grid, a backend with GPU programs runs the shader straight
  // on the target's pixels (no image); everywhere else the CPU renders an
  // image into the cache.
  inline bool shader_paint(neui_painter_t* p, const ShaderAsset& sa, ShaderCache& cache,
                           float x, float y, float w, float h,
                           float resolution, float time, const float* uniforms)
  {
    namespace nsl = neui::nsl;
    if (!p || !p->backend || !p->ctx || !sa.program || !p->backend->draw_bitmap) return false;
    if (!(w > 0.0f) || !(h > 0.0f)) return false;
    const nsl::Program& prog = *sa.program;

    const ShaderGrid g = shader_grid(p, shader_resolution_clamp(resolution));
    if (!(g.k > 0.0f) || !std::isfinite(g.k)) return false;

    // The layer origin on the shader grid, as whole px plus a remainder. The
    // remainder is snapped to 1/256 px, so float noise in a transform does
    // not split the cache (whole logical px at 150% give exactly 0 or 0.5).
    const float X0 = g.k * x + g.ox;
    const float Y0 = g.k * y + g.oy;
    if (!(std::fabs(X0) < 8388608.0f) || !(std::fabs(Y0) < 8388608.0f)) return false;
    float I = std::floor(X0), J = std::floor(Y0);
    float fx = std::floor((X0 - I) * 256.0f + 0.5f) / 256.0f;
    float fy = std::floor((Y0 - J) * 256.0f + 0.5f) / 256.0f;
    if (fx >= 1.0f) { fx = 0.0f; I += 1.0f; }
    if (fy >= 1.0f) { fy = 0.0f; J += 1.0f; }

    const size_t n = static_cast<size_t>(nsl::uniform_components(prog));
    std::vector<float> defaults;
    if (n && !uniforms) {
      defaults.resize(n);
      nsl::default_uniforms(prog, defaults.data());
      uniforms = defaults.data();
    }

    nsl::Draw d;
    d.x = fx / g.k;
    d.y = fy / g.k;
    d.w = w;
    d.h = h;
    d.scale = g.k;
    d.time = (nsl::flags(prog) & nsl::NSL_READS_TIME) ? time : 0.0f;
    d.uniforms = n ? uniforms : nullptr;

    if (g.device) {
      if (void* gp = cache.gpu_program(sa, p->backend)) {
        const nsl::Placement pl = nsl::placement(d);
        std::vector<float> cbuf(nsl::gpu_constant_count(prog));
        if (pl.w > 0 && pl.h > 0 && nsl::gpu_constants(prog, d, cbuf.data()) &&
            p->backend->draw_shader_program(p->ctx, gp,
                                            static_cast<int32_t>(I) + pl.x,
                                            static_cast<int32_t>(J) + pl.y,
                                            static_cast<uint32_t>(pl.w),
                                            static_cast<uint32_t>(pl.h), cbuf.data(),
                                            static_cast<uint32_t>(cbuf.size())))
          return true;
      }
    }

    ShaderCacheKey key;
    key.uid = sa.uid;
    key.w = w;
    key.h = h;
    key.scale = g.k;
    key.fx = fx;
    key.fy = fy;
    key.time = d.time;
    key.uniforms = d.uniforms;
    key.uniform_count = n;
    ShaderCacheEntry* e = cache.get(key, [&](ShaderCacheEntry& out) {
      const nsl::Placement pl = nsl::placement(d);
      if (pl.w <= 0 || pl.h <= 0) return false;
      out.w_px = static_cast<uint32_t>(pl.w);
      out.h_px = static_cast<uint32_t>(pl.h);
      out.px = pl.x;
      out.py = pl.y;
      const size_t stride = static_cast<size_t>(pl.w) * 4u;
      out.pixels.assign(stride * static_cast<size_t>(pl.h), 0);
      return nsl::execute(prog, d, out.pixels.data(), stride);
    }, p->backend);
    if (!e) return false;
    void* bmp = ShaderCache::upload(*e, p->backend, p->ctx);
    if (!bmp) return false;

    // Back from shader px to the painter's space.
    const float inv = 1.0f / g.k;
    p->backend->draw_bitmap(p->ctx, bmp, 0.0f, 0.0f, 0.0f, 0.0f,
                            (I + static_cast<float>(e->px) - g.ox) * inv,
                            (J + static_cast<float>(e->py) - g.oy) * inv,
                            static_cast<float>(e->w_px) * inv,
                            static_cast<float>(e->h_px) * inv, 0xFFFFFFFFu);
    return true;
  }

  // ---- The compound SHADER layer -------------------------------------------

  // True when `key` holds an int attribute: the strict getter answers only
  // for its own kind, so two different defaults agree only on a real int.
  inline bool attr_is_int(const AttrBag* bag, const std::string& key)
  {
    return bag && bag->get_int(key, 0) == bag->get_int(key, 1);
  }

  // Straight RGB(A) of byte / 255 from ARGB: the language's colour rule.
  inline void shader_unpack_argb(uint32_t argb, int comps, float* dst)
  {
    dst[0] = static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    dst[1] = static_cast<float>((argb >> 8) & 0xFFu) / 255.0f;
    dst[2] = static_cast<float>(argb & 0xFFu) / 255.0f;
    if (comps > 3) dst[3] = static_cast<float>((argb >> 24) & 0xFFu) / 255.0f;
  }

  inline void shader_apply_static(const CompoundLayer::ShaderUniformStatic& s,
                                  int comps, float* dst)
  {
    using S = CompoundLayer::ShaderUniformStatic;
    switch (s.kind) {
      case S::INT:
        if (comps >= 3) { shader_unpack_argb(static_cast<uint32_t>(s.i), comps, dst); break; }
        for (int c = 0; c < comps; ++c) dst[c] = static_cast<float>(s.i);
        break;
      case S::VEC:
        for (int c = 0; c < comps && c < s.count; ++c) dst[c] = s.v[c];
        break;
      default:
        for (int c = 0; c < comps; ++c) dst[c] = s.v[0];
        break;
    }
  }

  inline float shader_static_component(const CompoundLayer::ShaderUniformStatic& s)
  {
    return s.kind == CompoundLayer::ShaderUniformStatic::INT ? static_cast<float>(s.i) : s.v[0];
  }

  // A binding on a whole uniform. A vec3 / vec4 bound to an int attr reads it
  // as a colour, raw (scale, offset and step do not apply); otherwise the
  // binding's value goes to every component.
  inline void shader_apply_binding(const CompoundBinding& b, const AttrBag* bag,
                                   int comps, float* dst)
  {
    if (comps >= 3 && attr_is_int(bag, b.attr_key)) {
      shader_unpack_argb(static_cast<uint32_t>(bag->get_int(b.attr_key, 0)), comps, dst);
      return;
    }
    const float v = eval_binding_float(b, bag);
    for (int c = 0; c < comps; ++c) dst[c] = v;
  }

  // The uniform values of a SHADER layer: the program's declared defaults,
  // then the "u.<name>" values, the bindings on them, the "u.<name>.<c>"
  // values and the bindings on those, in that order. Props naming no uniform
  // of this program are ignored.
  inline void shader_layer_uniforms(const CompoundLayer& L, const neui::nsl::Program& prog,
                                    const AttrBag* bag, std::vector<float>& out)
  {
    namespace nsl = neui::nsl;
    out.assign(static_cast<size_t>(nsl::uniform_components(prog)), 0.0f);
    if (out.empty()) return;
    nsl::default_uniforms(prog, out.data());
    const auto& decls = nsl::uniforms(prog);

    // The declaration a key "<name>" or "<name>.<c>" starting at `at` names.
    auto find = [&](const std::string& key, size_t at, int comp) -> const nsl::UniformDesc* {
      const size_t len = (comp >= 0 ? key.find('.', at) : key.size()) - at;
      for (const auto& u : decls)
        if (u.name.size() == len && key.compare(at, len, u.name) == 0)
          return comp < u.components ? &u : nullptr;
      return nullptr;
    };

    for (int pass = 0; pass < 4; ++pass) {
      const bool components = pass >= 2;
      if (pass % 2 == 0) {
        for (const auto& kv : L.shader_uniforms) {
          const int c = shader_uniform_component(kv.first);
          if ((c >= 0) != components) continue;
          const nsl::UniformDesc* u = find(kv.first, 0, c);
          if (!u) continue;
          float* dst = &out[static_cast<size_t>(u->offset)];
          if (c < 0) shader_apply_static(kv.second, u->components, dst);
          else       dst[c] = shader_static_component(kv.second);
        }
      } else {
        for (const auto& kv : L.bindings) {
          const std::string& prop = kv.first;
          if (kv.second.is_asset || prop.size() < 3 || prop.compare(0, 2, "u.") != 0) continue;
          const size_t dot = prop.find('.', 2);
          const int c = dot == std::string::npos ? -1 : shader_uniform_component(prop.substr(2));
          if (dot != std::string::npos && c < 0) continue;
          if ((c >= 0) != components) continue;
          const nsl::UniformDesc* u = find(prop, 2, c);
          if (!u) continue;
          float* dst = &out[static_cast<size_t>(u->offset)];
          if (c < 0) shader_apply_binding(kv.second, bag, u->components, dst);
          else       dst[c] = eval_binding_float(kv.second, bag);
        }
      }
    }
  }

  // Resolves a SHADER layer's program and cache. False when the layer cannot
  // render here: no shader support (no thunk), or no valid shader asset.
  inline bool shader_layer_resolve(neui_painter_t* p, const CompoundLayer& L, const AttrBag* bag,
                                   const ShaderAsset** sa, ShaderCache** cache)
  {
    if (!p || !p->resolve_shader) return false;
    const neui_asset_t a = effective_asset(L, "shader", L.shader, bag);
    if (a.id == asset_none.id) return false;
    *sa = nullptr;
    *cache = nullptr;
    return p->resolve_shader(p->host_token, a, sa, cache) && *sa && (*sa)->program && *cache;
  }

  inline void paint_shader_layer(neui_painter_t* p, const CompoundLayer& L,
                                 float x, float y, float w, float h, const AttrBag* bag)
  {
    const ShaderAsset* sa = nullptr;
    ShaderCache* cache = nullptr;
    if (!shader_layer_resolve(p, L, bag, &sa, &cache)) return;
    std::vector<float> values;
    shader_layer_uniforms(L, *sa->program, bag, values);
    const float res = effective_float(L, "resolution", L.shader_resolution, bag);
    const float t = neui::nsl::wrap_time(effective_float(L, "time", L.shader_time, bag));
    shader_paint(p, *sa, *cache, x, y, w, h, res, t, values.empty() ? nullptr : values.data());
  }

  // True when the layer at `slot` is the fallback of a SHADER layer that can
  // render, so the fallback stays hidden.
  inline bool shader_fallback_hidden(neui_painter_t* p, const CompoundAsset& ca,
                                     uint32_t slot, const AttrBag* bag)
  {
    for (const auto& layer : ca.layers) {
      if (!layer || layer->kind != NEUI_COMPOUND_LAYER_SHADER || layer->fallback != slot) continue;
      const ShaderAsset* sa = nullptr;
      ShaderCache* cache = nullptr;
      if (shader_layer_resolve(p, *layer, bag, &sa, &cache)) return true;
    }
    return false;
  }

  // ---- NEUI_API_SHADER draw -------------------------------------------------

  inline bool NEUI_ABI shader_api_draw(neui_painter_t* p, const neui_shader_draw_t* d)
  {
    if (!p || !d || !p->resolve_shader) return false;
    const ShaderAsset* sa = nullptr;
    ShaderCache* cache = nullptr;
    if (!p->resolve_shader(p->host_token, d->shader, &sa, &cache) || !sa || !sa->program || !cache)
      return false;
    std::vector<float> values;
    shader_uniform_values(*sa->program, d->uniforms, d->uniform_count, values);
    return shader_paint(p, *sa, *cache, d->x, d->y, d->width, d->height, d->resolution,
                        neui::nsl::wrap_time(d->time), values.empty() ? nullptr : values.data());
  }
}  // namespace neui_detail
