#pragma once

// SVG filter-graph model + CPU evaluator, shared by all hosts.
//
// A FilterAsset is an ordered list of atomic SVG fe* primitives (the model
// behind NEUI_API_FILTER). Each primitive reads one or two named inputs,
// computes a premultiplied-BGRA8 result, and optionally registers it under a
// result name. evaluate_filter() runs the graph over a SURFACE's pixel buffer
// in place. This is the rendering half of the SVG <filter> element; the
// client (or a future SVG parser) describes the graph, neui evaluates it.
//
// Pixel format: premultiplied BGRA8, bytes [B, G, R, A]. Most primitives work
// in premultiplied space (blur / offset / composite / blend / merge / flood);
// feColorMatrix un-premultiplies, transforms straight RGBA, then re-
// premultiplies, per the SVG spec. Colour math is sRGB (no linearisation);
// linearRGB is deferred.
//
// Header-only / inline; the model structs mirror compound.h / behavior.h.
// Tier-1 unit-tested in tests/test_filter_graph.cpp.

#include <cstdint>
#include <cstring>
#include <cmath>
#include <string>
#include <vector>
#include <memory>
#include <unordered_map>
#include <algorithm>

#include <neui/d/filter.h>     // neui_filter_prim_kind_t + handle
#include "image_filter.h"      // image_gaussian_blur_bgra

namespace neui_detail {

  // ---- Handle pack/unpack (mirrors compound_layer_*) -----------------------
  inline uint32_t filter_prim_asset_slot(neui_filter_prim_t p) { return (p.id >> 16) & 0xffff; }
  inline uint32_t filter_prim_slot(neui_filter_prim_t p)       { return p.id & 0xffff; }
  inline neui_filter_prim_t pack_filter_prim(uint32_t asset_slot, uint32_t prim_slot)
  { return { ((asset_slot & 0xffff) << 16) | (prim_slot & 0xffff) }; }

  // ---- Model ---------------------------------------------------------------

  struct FilterPrimitive
  {
    neui_filter_prim_kind_t kind = NEUI_FE_NONE;
    std::string in;     // input slot 0
    std::string in2;    // input slot 1
    std::string result; // registered output name (empty = unnamed)

    bool  has_region = false;
    float rx = 0, ry = 0, rw = 0, rh = 0;  // logical px

    // feFlood
    uint32_t flood_color   = 0xFF000000u;
    float    flood_opacity = 1.0f;

    // feColorMatrix
    std::string        cm_type = "matrix";
    std::vector<float> values;

    // feOffset
    float dx = 0, dy = 0;

    // feGaussianBlur
    float sigma_x = 0, sigma_y = 0;

    // feComposite
    std::string composite_op = "over";
    float k1 = 0, k2 = 0, k3 = 0, k4 = 0;

    // feBlend
    std::string blend_mode = "normal";

    // feMerge
    std::vector<std::string> merge_inputs;
  };

  struct FilterAsset
  {
    std::vector<std::unique_ptr<FilterPrimitive>> prims;
    std::vector<uint32_t>                         free_slots;
  };

  // ---- Model mutators (mirror compound_add_layer / apply_set_*) ------------

  inline uint32_t filter_add_primitive(FilterAsset& fa, neui_filter_prim_kind_t kind)
  {
    auto p = std::make_unique<FilterPrimitive>();
    p->kind = kind;
    if (!fa.free_slots.empty()) {
      uint32_t slot = fa.free_slots.back();
      fa.free_slots.pop_back();
      fa.prims[slot] = std::move(p);
      return slot;
    }
    fa.prims.push_back(std::move(p));
    return static_cast<uint32_t>(fa.prims.size() - 1);
  }

  inline FilterPrimitive* filter_get_prim(FilterAsset& fa, uint32_t slot)
  {
    if (slot >= fa.prims.size()) return nullptr;
    return fa.prims[slot].get();
  }

  inline void filter_remove_primitive(FilterAsset& fa, uint32_t slot)
  {
    if (slot >= fa.prims.size() || !fa.prims[slot]) return;
    fa.prims[slot].reset();
    fa.free_slots.push_back(slot);
  }

  inline void filter_clear(FilterAsset& fa)
  {
    fa.prims.clear();
    fa.free_slots.clear();
  }

  inline void apply_filter_set_input(FilterPrimitive& P, int slot, const char* src)
  {
    const std::string s = src ? src : "";
    if (slot == 1) P.in2 = s; else P.in = s;
  }
  inline void apply_filter_set_result(FilterPrimitive& P, const char* name)
  { P.result = name ? name : ""; }
  inline void apply_filter_set_region(FilterPrimitive& P, float x, float y, float w, float h)
  { P.has_region = (w > 0.0f && h > 0.0f); P.rx = x; P.ry = y; P.rw = w; P.rh = h; }

  inline void apply_filter_set_int(FilterPrimitive& P, const std::string& prop, int v)
  {
    if (prop == "flood_color") P.flood_color = static_cast<uint32_t>(v);
  }
  inline void apply_filter_set_float(FilterPrimitive& P, const std::string& prop, float v)
  {
    if      (prop == "flood_opacity") P.flood_opacity = v;
    else if (prop == "dx")            P.dx = v;
    else if (prop == "dy")            P.dy = v;
    else if (prop == "std_dev")     { P.sigma_x = v; P.sigma_y = v; }
    else if (prop == "std_dev_x")     P.sigma_x = v;
    else if (prop == "std_dev_y")     P.sigma_y = v;
    else if (prop == "k1")            P.k1 = v;
    else if (prop == "k2")            P.k2 = v;
    else if (prop == "k3")            P.k3 = v;
    else if (prop == "k4")            P.k4 = v;
  }
  inline void apply_filter_set_string(FilterPrimitive& P, const std::string& prop, const char* v)
  {
    const std::string s = v ? v : "";
    if      (prop == "type")     P.cm_type = s;
    else if (prop == "operator") P.composite_op = s;
    else if (prop == "mode")     P.blend_mode = s;
  }
  inline void apply_filter_set_floats(FilterPrimitive& P, const std::string& prop,
                                      const float* values, uint32_t count)
  {
    if (prop != "values") return;
    P.values.assign(values && count ? values : nullptr,
                    values && count ? values + count : nullptr);
  }
  inline void apply_filter_merge_add_input(FilterPrimitive& P, const char* src)
  { if (P.kind == NEUI_FE_MERGE && src) P.merge_inputs.emplace_back(src); }

  // ---- Convenience recipe builders -----------------------------------------
  // Each appends primitives to `fa` to express a common multi-primitive effect
  // (the AssetStore surface_* convenience methods + the tests both build via
  // these, so there is one recipe home). All distances are in LOGICAL px
  // (evaluate_filter scales by the surface's backing scale).

  // Fill a 4x5 colour matrix that recolours to `argb` (RGB from the bias
  // column, output alpha = (argb alpha * alpha_scale) * source alpha). Applied
  // to SourceAlpha it makes a coverage-shaped tint; to SourceGraphic it
  // colourises (replaces RGB, keeps coverage).
  inline void fg_tint_matrix(float m[20], uint32_t argb, float alpha_scale = 1.0f)
  {
    const float a = (static_cast<float>((argb >> 24) & 0xFFu) / 255.0f) * alpha_scale;
    const float r =  static_cast<float>((argb >> 16) & 0xFFu) / 255.0f;
    const float g =  static_cast<float>((argb >> 8)  & 0xFFu) / 255.0f;
    const float b =  static_cast<float>((argb)       & 0xFFu) / 255.0f;
    const float src[20] = { 0,0,0,0,r,  0,0,0,0,g,  0,0,0,0,b,  0,0,0,a,0 };
    for (int i = 0; i < 20; ++i) m[i] = src[i];
  }

  inline void filter_build_blur(FilterAsset& fa, float sigma_x, float sigma_y)
  {
    auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_GAUSSIAN_BLUR));
    P->sigma_x = sigma_x; P->sigma_y = sigma_y;
  }

  // feDropShadow: tint(SourceAlpha) -> offset -> blur -> over SourceGraphic.
  inline void filter_build_drop_shadow(FilterAsset& fa, float dx, float dy,
                                       float sigma, uint32_t argb)
  {
    float m[20]; fg_tint_matrix(m, argb);
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_COLOR_MATRIX));
      P->in = "SourceAlpha"; P->cm_type = "matrix"; P->values.assign(m, m + 20); }
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_OFFSET));
      P->dx = dx; P->dy = dy; }
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_GAUSSIAN_BLUR));
      P->sigma_x = sigma; P->sigma_y = sigma; }
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_COMPOSITE));
      P->composite_op = "over"; P->in = "SourceGraphic"; }
  }

  // Outer glow = a zero-offset, coloured drop shadow.
  inline void filter_build_glow(FilterAsset& fa, float sigma, uint32_t argb)
  { filter_build_drop_shadow(fa, 0.0f, 0.0f, sigma, argb); }

  // Colourise: replace RGB with `argb`, keep coverage (argb alpha scales it).
  inline void filter_build_tint(FilterAsset& fa, uint32_t argb)
  {
    float m[20]; fg_tint_matrix(m, argb);
    auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_COLOR_MATRIX));
    P->cm_type = "matrix"; P->values.assign(m, m + 20);
  }

  // Desaturate: amount 1 = fully grey, 0 = unchanged (saturate(1-amount)).
  inline void filter_build_desaturate(FilterAsset& fa, float amount)
  {
    float s = 1.0f - (amount < 0.0f ? 0.0f : (amount > 1.0f ? 1.0f : amount));
    auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_COLOR_MATRIX));
    P->cm_type = "saturate"; P->values = { s };
  }

  // Material-style elevation: a wide soft ambient shadow + a tighter key
  // shadow, both black, under the source.
  inline void filter_build_elevation(FilterAsset& fa, float level)
  {
    float ma[20], mk[20];
    fg_tint_matrix(ma, 0x1F000000u);  // ambient ~12%
    fg_tint_matrix(mk, 0x3D000000u);  // key ~24%
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_COLOR_MATRIX));
      P->in = "SourceAlpha"; P->cm_type = "matrix"; P->values.assign(ma, ma + 20); }
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_OFFSET)); P->dy = level * 0.5f; }
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_GAUSSIAN_BLUR));
      P->sigma_x = P->sigma_y = level * 1.2f; P->result = "amb"; }
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_COLOR_MATRIX));
      P->in = "SourceAlpha"; P->cm_type = "matrix"; P->values.assign(mk, mk + 20); }
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_OFFSET)); P->dy = level * 0.3f; }
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_GAUSSIAN_BLUR));
      P->sigma_x = P->sigma_y = level * 0.4f; P->result = "key"; }
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_MERGE));
      P->merge_inputs = { "amb", "key", "SourceGraphic" }; }
  }

  // Helper: an inner-shadow band tinted by `argb`, clipped to the shape,
  // registered under `result_name`. band = SourceAlpha - blur(offset(SourceAlpha))
  // (feComposite arithmetic), tinted, then composited "in" SourceGraphic.
  inline void filter_build_inner_band(FilterAsset& fa, float dx, float dy,
                                      float sigma, uint32_t argb, const char* result_name)
  {
    float m[20]; fg_tint_matrix(m, argb);
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_OFFSET));
      P->in = "SourceAlpha"; P->dx = dx; P->dy = dy; }
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_GAUSSIAN_BLUR));
      P->sigma_x = P->sigma_y = sigma; }
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_COMPOSITE));
      P->composite_op = "arithmetic"; P->in2 = "SourceAlpha"; P->k2 = -1.0f; P->k3 = 1.0f; }
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_COLOR_MATRIX));
      P->cm_type = "matrix"; P->values.assign(m, m + 20); }
    { auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_COMPOSITE));
      P->composite_op = "in"; P->in2 = "SourceGraphic"; P->result = result_name; }
  }

  // Inner shadow: an inset shadow band over the source.
  inline void filter_build_inner_shadow(FilterAsset& fa, float dx, float dy,
                                        float sigma, uint32_t argb)
  {
    filter_build_inner_band(fa, dx, dy, sigma, argb, "ishadow");
    auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_MERGE));
    P->merge_inputs = { "SourceGraphic", "ishadow" };
  }

  // Bevel: a light inner band from one corner + a dark inner band from the
  // opposite corner, both clipped to the shape, over the source.
  inline void filter_build_bevel(FilterAsset& fa, float dx, float dy, float sigma,
                                 uint32_t light_argb, uint32_t dark_argb)
  {
    filter_build_inner_band(fa, -dx, -dy, sigma, light_argb, "lightband");
    filter_build_inner_band(fa,  dx,  dy, sigma, dark_argb,  "darkband");
    auto* P = filter_get_prim(fa, filter_add_primitive(fa, NEUI_FE_MERGE));
    P->merge_inputs = { "SourceGraphic", "lightband", "darkband" };
  }

  // ---- Pixel-op helpers (premultiplied BGRA8) ------------------------------

  inline float fg_clamp01(float x) { return x < 0.0f ? 0.0f : (x > 1.0f ? 1.0f : x); }
  inline uint8_t fg_to_byte(float x01) { return static_cast<uint8_t>(fg_clamp01(x01) * 255.0f + 0.5f); }

  inline void fg_flood(std::vector<uint8_t>& out, uint32_t w, uint32_t h,
                       const FilterPrimitive& P)
  {
    const float a8 = static_cast<float>((P.flood_color >> 24) & 0xFFu);
    const float a  = (a8 / 255.0f) * fg_clamp01(P.flood_opacity);
    const float R  = static_cast<float>((P.flood_color >> 16) & 0xFFu) / 255.0f;
    const float G  = static_cast<float>((P.flood_color >> 8)  & 0xFFu) / 255.0f;
    const float B  = static_cast<float>((P.flood_color)       & 0xFFu) / 255.0f;
    const uint8_t pb = fg_to_byte(B * a), pg = fg_to_byte(G * a),
                  pr = fg_to_byte(R * a), pa = fg_to_byte(a);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
      out[i * 4 + 0] = pb; out[i * 4 + 1] = pg; out[i * 4 + 2] = pr; out[i * 4 + 3] = pa;
    }
  }

  inline void fg_offset(std::vector<uint8_t>& out, const std::vector<uint8_t>& in,
                        uint32_t w, uint32_t h, const FilterPrimitive& P, float scale)
  {
    const int dx = static_cast<int>(std::lround(P.dx * scale));
    const int dy = static_cast<int>(std::lround(P.dy * scale));
    for (uint32_t y = 0; y < h; ++y) {
      const int sy = static_cast<int>(y) - dy;
      if (sy < 0 || sy >= static_cast<int>(h)) continue;
      for (uint32_t x = 0; x < w; ++x) {
        const int sx = static_cast<int>(x) - dx;
        if (sx < 0 || sx >= static_cast<int>(w)) continue;
        std::memcpy(&out[(static_cast<size_t>(y) * w + x) * 4],
                    &in[(static_cast<size_t>(sy) * w + sx) * 4], 4);
      }
    }
  }

  // Build the 4x5 matrix for a feColorMatrix primitive (rows R,G,B,A; col 5 = bias).
  inline void fg_build_color_matrix(const FilterPrimitive& P, float m[20])
  {
    // identity
    static const float kIdent[20] = {
      1,0,0,0,0,  0,1,0,0,0,  0,0,1,0,0,  0,0,0,1,0 };
    std::memcpy(m, kIdent, sizeof(kIdent));
    if (P.cm_type == "matrix") {
      if (P.values.size() >= 20) for (int i = 0; i < 20; ++i) m[i] = P.values[i];
    } else if (P.cm_type == "saturate") {
      const float s = P.values.empty() ? 1.0f : P.values[0];
      const float r[20] = {
        0.213f + 0.787f * s, 0.715f - 0.715f * s, 0.072f - 0.072f * s, 0, 0,
        0.213f - 0.213f * s, 0.715f + 0.285f * s, 0.072f - 0.072f * s, 0, 0,
        0.213f - 0.213f * s, 0.715f - 0.715f * s, 0.072f + 0.928f * s, 0, 0,
        0, 0, 0, 1, 0 };
      std::memcpy(m, r, sizeof(r));
    } else if (P.cm_type == "hueRotate") {
      const float deg = P.values.empty() ? 0.0f : P.values[0];
      const float a = deg * 3.14159265358979f / 180.0f;
      const float c = std::cos(a), s = std::sin(a);
      const float r[20] = {
        0.213f + c * 0.787f - s * 0.213f, 0.715f - c * 0.715f - s * 0.715f, 0.072f - c * 0.072f + s * 0.928f, 0, 0,
        0.213f - c * 0.213f + s * 0.143f, 0.715f + c * 0.285f + s * 0.140f, 0.072f - c * 0.072f - s * 0.283f, 0, 0,
        0.213f - c * 0.213f - s * 0.787f, 0.715f - c * 0.715f + s * 0.715f, 0.072f + c * 0.928f + s * 0.072f, 0, 0,
        0, 0, 0, 1, 0 };
      std::memcpy(m, r, sizeof(r));
    } else if (P.cm_type == "luminanceToAlpha") {
      const float r[20] = {
        0, 0, 0, 0, 0,
        0, 0, 0, 0, 0,
        0, 0, 0, 0, 0,
        0.2125f, 0.7154f, 0.0721f, 0, 0 };
      std::memcpy(m, r, sizeof(r));
    }
  }

  inline void fg_color_matrix(std::vector<uint8_t>& out, const std::vector<uint8_t>& in,
                              uint32_t w, uint32_t h, const FilterPrimitive& P)
  {
    float m[20];
    fg_build_color_matrix(P, m);
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
      const float a = in[i * 4 + 3] / 255.0f;
      // un-premultiply to straight 0..1
      float R = 0, G = 0, B = 0;
      if (a > 0.0f) {
        B = (in[i * 4 + 0] / 255.0f) / a;
        G = (in[i * 4 + 1] / 255.0f) / a;
        R = (in[i * 4 + 2] / 255.0f) / a;
      }
      const float nr = fg_clamp01(m[0]  * R + m[1]  * G + m[2]  * B + m[3]  * a + m[4]);
      const float ng = fg_clamp01(m[5]  * R + m[6]  * G + m[7]  * B + m[8]  * a + m[9]);
      const float nb = fg_clamp01(m[10] * R + m[11] * G + m[12] * B + m[13] * a + m[14]);
      const float na = fg_clamp01(m[15] * R + m[16] * G + m[17] * B + m[18] * a + m[19]);
      // re-premultiply
      out[i * 4 + 0] = fg_to_byte(nb * na);
      out[i * 4 + 1] = fg_to_byte(ng * na);
      out[i * 4 + 2] = fg_to_byte(nr * na);
      out[i * 4 + 3] = fg_to_byte(na);
    }
  }

  inline void fg_composite(std::vector<uint8_t>& out,
                           const std::vector<uint8_t>& src, const std::vector<uint8_t>& dst,
                           uint32_t w, uint32_t h, const FilterPrimitive& P)
  {
    // Prefixed names: the Windows SDK defines IN / OUT as macros.
    enum { OP_OVER, OP_IN, OP_OUT, OP_ATOP, OP_XOR, OP_ARITH } op = OP_OVER;
    if      (P.composite_op == "in")         op = OP_IN;
    else if (P.composite_op == "out")        op = OP_OUT;
    else if (P.composite_op == "atop")       op = OP_ATOP;
    else if (P.composite_op == "xor")        op = OP_XOR;
    else if (P.composite_op == "arithmetic") op = OP_ARITH;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
      const float sa = src[i * 4 + 3] / 255.0f, da = dst[i * 4 + 3] / 255.0f;
      for (int c = 0; c < 4; ++c) {
        const float s = src[i * 4 + c] / 255.0f, d = dst[i * 4 + c] / 255.0f;
        float o = 0.0f;
        switch (op) {
          case OP_OVER:  o = s + d * (1.0f - sa); break;
          case OP_IN:    o = s * da; break;
          case OP_OUT:   o = s * (1.0f - da); break;
          case OP_ATOP:  o = s * da + d * (1.0f - sa); break;
          case OP_XOR:   o = s * (1.0f - da) + d * (1.0f - sa); break;
          case OP_ARITH: o = P.k1 * s * d + P.k2 * s + P.k3 * d + P.k4; break;
        }
        out[i * 4 + c] = fg_to_byte(o);
      }
    }
  }

  inline void fg_blend(std::vector<uint8_t>& out,
                       const std::vector<uint8_t>& src, const std::vector<uint8_t>& dst,
                       uint32_t w, uint32_t h, const FilterPrimitive& P)
  {
    enum { BL_NORMAL, BL_MULTIPLY, BL_SCREEN, BL_DARKEN, BL_LIGHTEN } mode = BL_NORMAL;
    if      (P.blend_mode == "multiply") mode = BL_MULTIPLY;
    else if (P.blend_mode == "screen")   mode = BL_SCREEN;
    else if (P.blend_mode == "darken")   mode = BL_DARKEN;
    else if (P.blend_mode == "lighten")  mode = BL_LIGHTEN;
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
      const float as = src[i * 4 + 3] / 255.0f, ab = dst[i * 4 + 3] / 255.0f;
      const float ao = as + ab * (1.0f - as);
      out[i * 4 + 3] = fg_to_byte(ao);
      for (int c = 0; c < 3; ++c) {
        const float csp = src[i * 4 + c] / 255.0f;   // premultiplied
        const float cbp = dst[i * 4 + c] / 255.0f;
        const float Cs = as > 0.0f ? csp / as : 0.0f;  // straight
        const float Cb = ab > 0.0f ? cbp / ab : 0.0f;
        float Bv = Cs;
        switch (mode) {
          case BL_NORMAL:   Bv = Cs; break;
          case BL_MULTIPLY: Bv = Cs * Cb; break;
          case BL_SCREEN:   Bv = Cs + Cb - Cs * Cb; break;
          case BL_DARKEN:   Bv = std::min(Cs, Cb); break;
          case BL_LIGHTEN:  Bv = std::max(Cs, Cb); break;
        }
        const float cr = (1.0f - ab) * csp + (1.0f - as) * cbp + as * ab * Bv;
        out[i * 4 + c] = fg_to_byte(cr);
      }
    }
  }

  // out = src OVER out (accumulate, premultiplied).
  inline void fg_over_inplace(std::vector<uint8_t>& acc, const std::vector<uint8_t>& src,
                              uint32_t w, uint32_t h)
  {
    for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) {
      const float sa = src[i * 4 + 3] / 255.0f;
      for (int c = 0; c < 4; ++c) {
        const float s = src[i * 4 + c] / 255.0f, d = acc[i * 4 + c] / 255.0f;
        acc[i * 4 + c] = fg_to_byte(s + d * (1.0f - sa));
      }
    }
  }

  inline void fg_region_clip(std::vector<uint8_t>& buf, uint32_t w, uint32_t h,
                             const FilterPrimitive& P, float scale)
  {
    const int x0 = static_cast<int>(std::lround(P.rx * scale));
    const int y0 = static_cast<int>(std::lround(P.ry * scale));
    const int x1 = x0 + static_cast<int>(std::lround(P.rw * scale));
    const int y1 = y0 + static_cast<int>(std::lround(P.rh * scale));
    for (uint32_t y = 0; y < h; ++y)
      for (uint32_t x = 0; x < w; ++x) {
        if (static_cast<int>(x) >= x0 && static_cast<int>(x) < x1 &&
            static_cast<int>(y) >= y0 && static_cast<int>(y) < y1) continue;
        std::memset(&buf[(static_cast<size_t>(y) * w + x) * 4], 0, 4);
      }
  }

  // ---- Graph evaluator -----------------------------------------------------

  // Evaluate `fa` in place over premultiplied BGRA8 `px` (w x h physical px).
  // `scale` converts logical-px params (offset / blur / region) to physical.
  inline void evaluate_filter(const FilterAsset& fa, uint8_t* px,
                              uint32_t w, uint32_t h, float scale)
  {
    if (!px || w == 0 || h == 0) return;
    const size_t n4 = static_cast<size_t>(w) * h * 4u;

    std::vector<const FilterPrimitive*> prims;
    for (const auto& up : fa.prims)
      if (up && up->kind != NEUI_FE_NONE) prims.push_back(up.get());
    if (prims.empty()) return;

    std::unordered_map<std::string, std::vector<uint8_t>> results;
    results["SourceGraphic"] = std::vector<uint8_t>(px, px + n4);
    {
      std::vector<uint8_t> sa(n4, 0);
      for (size_t i = 0; i < static_cast<size_t>(w) * h; ++i) sa[i * 4 + 3] = px[i * 4 + 3];
      results["SourceAlpha"] = std::move(sa);
    }

    std::vector<uint8_t> last;
    bool have_last = false;
    auto resolve = [&](const std::string& name) -> const std::vector<uint8_t>& {
      if (!name.empty()) {
        auto it = results.find(name);
        if (it != results.end()) return it->second;
      }
      if (have_last) return last;
      return results["SourceGraphic"];
    };

    for (const FilterPrimitive* pp : prims) {
      const FilterPrimitive& P = *pp;
      std::vector<uint8_t> out(n4, 0);
      switch (P.kind) {
        case NEUI_FE_FLOOD:         fg_flood(out, w, h, P); break;
        case NEUI_FE_OFFSET:        fg_offset(out, resolve(P.in), w, h, P, scale); break;
        case NEUI_FE_GAUSSIAN_BLUR: out = resolve(P.in);
                                    image_gaussian_blur_bgra(out.data(), w, h,
                                                             P.sigma_x * scale, P.sigma_y * scale);
                                    break;
        case NEUI_FE_COLOR_MATRIX:  fg_color_matrix(out, resolve(P.in), w, h, P); break;
        case NEUI_FE_COMPOSITE:     fg_composite(out, resolve(P.in), resolve(P.in2), w, h, P); break;
        case NEUI_FE_BLEND:         fg_blend(out, resolve(P.in), resolve(P.in2), w, h, P); break;
        case NEUI_FE_MERGE:
          for (const std::string& src : P.merge_inputs)
            fg_over_inplace(out, resolve(src), w, h);
          break;
        default: break;
      }
      if (P.has_region) fg_region_clip(out, w, h, P, scale);
      if (!P.result.empty()) results[P.result] = out;
      last = std::move(out);
      have_last = true;
    }

    std::memcpy(px, last.data(), n4);
  }

} // namespace neui_detail
