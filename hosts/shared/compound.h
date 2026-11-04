#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <memory>
#include <string>
#include <unordered_map>
#include <vector>

#include <neui/neui.h>

#include "attrs.h"

// Compound drawable - internal data model. Shared between the xpl and win32
// native hosts. Owns:
//   - a slot-reused layer table (CompoundAsset),
//   - per-layer geometry (anchors, offsets, size with FILL sentinels),
//   - per-layer typed properties (text-layer: text/size/color/align_*;
//     asset-layer: asset/rotation; both: alpha),
//   - per-layer bindings (numeric + asset),
//   - preparsed text-template segments.
//
// Mutator helpers (apply_set_int / apply_bind / ...) operate on a
// CompoundLayer; the host-side neui_compound_api_t thunks resolve the
// asset + layer handle through their session's AssetManager and then
// dispatch here. Paint-time evaluation (effective_int / effective_float /
// effective_asset / resolve_template / resolve_layer_rect) is also here
// so widget_paint_compound.h can stay platform-agnostic.

namespace neui_detail
{
  // A binding ties a layer property to a widget attribute. For numeric
  // bindings the runtime computes scale*x + offset where x is the
  // attribute read as float (int attrs are promoted). For asset
  // bindings the attribute is read as int and reinterpreted as the
  // .id of a neui_asset_t.
  struct CompoundBinding
  {
    std::string attr_key;
    float       scale    = 1.0f;
    float       offset   = 0.0f;
    // Quantisation of the attribute value, applied BEFORE scale/offset: the
    // value is snapped to the nearest multiple of `step`. 0 (default) = smooth.
    // A stepped bar / arc / rotation is a display decision, not a data one, so
    // it belongs here rather than in whatever writes the attr.
    float       step     = 0.0f;
    bool        is_asset = false;
  };

  // A preparsed text template segment. Either a literal run of bytes or
  // an attribute key whose value is substituted at paint time. The full
  // template is a vector<TextSegment>; concatenating .text for literals
  // and the attr's string-representation for keys yields the rendered
  // string.
  struct TextSegment
  {
    bool        is_literal = true;
    std::string text;       // literal text OR attr key when is_literal=false
  };

  struct CompoundLayer
  {
    neui_compound_layer_kind_t kind = NEUI_COMPOUND_LAYER_NONE;
    int                        z              = 0;
    // Owning GROUP layer's slot, or 0 (the unused sentinel slot) for a
    // top-level layer. A NEUI_COMPOUND_LAYER_GROUP layer collects every layer
    // whose `parent` equals its own slot; those children anchor / size against
    // the group rect and clip to it. See compound_add_child_layer.
    uint32_t                   parent         = 0;
    neui_anchor_t              parent_anchor  = NEUI_ANCHOR_TOP_LEFT;
    neui_anchor_t              self_anchor    = NEUI_ANCHOR_TOP_LEFT;

    // Geometry common to all kinds.
    int   offset_x = 0;
    int   offset_y = 0;
    int   width    = NEUI_COMPOUND_FILL;
    int   height   = NEUI_COMPOUND_FILL;
    float alpha    = 1.0f;
    // Layer state filter (NEUI_LAYER_STATE_* bitmask). 0 = always visible.
    // Non-zero = paint only when (widget_state & show_when) != 0.
    uint32_t show_when = 0;

    // Text-layer fields.
    std::string              text_template;       // raw source (the {key} string)
    std::vector<TextSegment> text_segments;       // preparsed
    float                    text_size    = 12.0f;
    // Text-layer font selection. Empty family / weight 0 leaves the
    // host's system default in place (Segoe UI Normal on D2D). Honoured
    // by the d2d backend; the cg backend ignores them (no-op push_font).
    // `family` is a template string (same {key} substitution as `text`),
    // pre-parsed at set_string time so paint can resolve it cheaply.
    std::string              text_family_template;
    std::vector<TextSegment> text_family_segments;
    int                      text_weight  = 0;    // CSS 100..900; 0 = Normal
    // text_color is optional - when neither set explicitly via set_int
    // ("color", ...) nor bound via bind("color", ...), the paint helper
    // falls back to the active theme's text_primary role. text_color_set
    // tracks the "explicitly set" state independently of the value so
    // the fallback works regardless of which ARGB the client picks.
    uint32_t                 text_color   = 0xFF000000;
    bool                     text_color_set = false;
    int                      text_align_x = 1;    // 0=start, 1=center, 2=end
    int                      text_align_y = 1;    // 0=top,   1=center, 2=bottom

    // Asset-layer fields.
    neui_asset_t             asset    = asset_none;
    float                    rotation = 0.0f;
    // Filmstrip cell to draw (row-major). Ignored when `asset` carries no
    // frame layout (an ordinary bitmap draws whole regardless). Default 0;
    // typically bound to a value attr (bind "frame" -> value, scale N-1) so
    // a knob/fader value scrubs the strip. Negative results clamp to 0.
    int                      frame    = 0;
    // Multiplicative ARGB tint applied to the bitmap's pixels. The
    // default 0xFFFFFFFFu is the passthrough sentinel - it short-circuits
    // the backend's tint primitive entirely so untinted draws cost
    // nothing extra. Any other value runs the backend's native
    // multiplicative-tint path (D2D effect on Windows, blend-mode
    // multiply + alpha mask on macOS); the source bitmap is cached
    // once per (asset, ctx) regardless of how many tints reference it.
    uint32_t                 tint     = 0xFFFFFFFFu;

    // Fill / stroke shared between RECT and PATH layers. Both colours
    // default to 0 (fully transparent), so a freshly-added shape with
    // no further setup is invisible - callers opt into a fill, a
    // stroke, or both. corner_radius is rect-only.
    uint32_t                 fill_color    = 0x00000000;
    uint32_t                 stroke_color  = 0x00000000;
    float                    stroke_width  = 0.0f;
    float                    corner_radius = 0.0f;

    // RECT value-driven fill (the linear bar / level meter - request §B). The
    // "value" prop is the painted FRACTION of the rect along the fill axis;
    // default 1 ⇒ full fill ⇒ an unbound RECT is unchanged. orientation picks
    // the axis (0 horizontal / 1 vertical); polarity (KnobPolarity values)
    // anchors the fill: 0 min/origin edge, 1 center/bipolar, 2 max/far edge.
    // The stroke (if any) always outlines the FULL rect as a track; the
    // gradient fill stays mapped to the full rect so it reads as a fixed scale
    // the fill reveals. Normally bound: bind("value","neui.param.value",1,0).
    float                    rect_value       = 1.0f;
    int                      rect_orientation = 0;
    int                      rect_polarity    = 0;

    // SVG fill-rule + stroke style for RECT / PATH layers (cap/join/dash only
    // affect PATH strokes). Defaults match the bare fill_path / stroke_path:
    // nonzero fill, butt cap, miter join, solid line. Stored as plain ints
    // (cast to the neui_*_t enums at paint) so the model needs no extra header.
    int                      fill_rule          = 0;     // 0 nonzero, 1 evenodd
    int                      stroke_cap         = 0;     // 0 butt, 1 round, 2 square
    int                      stroke_join        = 0;     // 0 miter, 1 round, 2 bevel
    float                    stroke_miter       = 4.0f;  // SVG default
    std::vector<float>       stroke_dash;                // on/off lengths (empty = solid)
    float                    stroke_dash_offset = 0.0f;

    // ARC-layer (NEUI_COMPOUND_LAYER_ARC) config. The arc is inscribed in the
    // layer rect (ellipse when width != height); `arc_value` (bindable, the
    // "value" prop) is the painted FRACTION of the begin..end range, anchored
    // by `arc_polarity` (KnobPolarity values). Angles are in degrees, 0 = 12
    // o'clock, positive = clockwise on screen. Reuses fill_color (pie) +
    // stroke_color / stroke_width / stroke_cap (ring) above.
    float                    arc_value     = 0.0f;     // [0..1] swept fraction
    float                    arc_begin_deg = -135.0f;  // range start (deg)
    float                    arc_end_deg   = 135.0f;   // range end   (deg)
    int                      arc_polarity  = 0;        // 0 min, 1 center, 2 max
    int                      arc_direction = 0;        // 0 cw, 1 ccw

    // Optional gradient fill for RECT / PATH layers (set via the compound
    // API's set_gradient). When `enabled` with >= 2 stops the layer's FILL
    // uses this gradient instead of the solid fill_color; the stroke is
    // unaffected. Geometry is stored as normalised [0,1] fractions of the
    // resolved layer rect (so it scales with the layer) and mapped to
    // absolute pixels at paint time; `radius` (radial only) is a fraction
    // of the layer rect's larger dimension. `stops` is the owned copy of
    // the client's stop array (the public neui_gradient_t::stops is
    // borrowed, so we snapshot it here, same as set_path snapshots cmds).
    struct GradientFill {
      bool                   enabled = false;
      neui_gradient_kind_t   kind    = NEUI_GRADIENT_LINEAR;
      neui_gradient_extend_t extend  = NEUI_GRADIENT_EXTEND_CLAMP;
      float start_x = 0.0f, start_y = 0.0f;   // default axis: top ...
      float end_x   = 0.0f, end_y   = 1.0f;   // ... to bottom (vertical)
      float radius  = 0.5f;                    // radial: fraction of max(w,h)
      std::vector<neui_gradient_stop_t> stops;
    };
    GradientFill             fill_gradient;

    // Path-layer geometry. Mirror of the public neui_path_cmd_t layout
    // so set_path can memcpy. Empty vector = no path (layer paints
    // nothing). Replayed in declaration order against the painter's
    // begin_path / move_to / line_to / arc / close_path primitives.
    struct PathCommand {
      uint32_t kind;
      float    args[6];   // six: a cubic Bézier needs two control points + end
    };
    std::vector<PathCommand> path_cmds;

    // PATH value-driven stroke trim (request §A). The "value" prop is the
    // trimmed FRACTION of the path's total arc length to stroke; default 1 ⇒
    // whole path ⇒ an unbound PATH is unchanged (byte-for-byte). trim_polarity
    // (KnobPolarity values) anchors the trimmed span: 0 min (from the start),
    // 1 center (grows out from the middle, bipolar), 2 max (from the end). The
    // trim is a geometry op on the STROKE only - fills always use the whole
    // path (a partial fill of an arbitrary path has no canonical meaning).
    // Normally bound: bind("value","neui.param.value",1,0).
    float                    trim_value    = 1.0f;
    int                      trim_polarity = 0;

    // QR-layer (NEUI_COMPOUND_LAYER_QR) config. The string to encode reuses
    // text_template / text_segments above (default "{value}"), unless the
    // widget's AttrBag carries a non-empty NEUI_ATTR_QRCODE string, which
    // wins. These knobs are part of the (shared) layer, so they apply to
    // every widget the compound backs.
    uint32_t qr_dark       = 0;           // dark-module ARGB; 0 = theme text_primary
    uint32_t qr_background = 0x00000000;  // ARGB behind the modules; 0 = transparent
    int      qr_ecc        = 1;           // neui_qr_ecc_t (0..3); default MEDIUM
    int      qr_quiet      = 4;           // quiet-zone width in modules

    // Per-(ctx) uploaded bitmap handle - same lazy pattern as
    // AssetEntry::bitmaps (CtxBitmap there; a local mirror here keeps
    // compound.h free of the asset_store.h dependency, which includes us).
    struct CtxBmp { void* bmp = nullptr; uint32_t generation = 0; };

    // A single rasterised QR symbol plus the generation key it was built
    // from and its per-(ctx) GPU uploads.
    struct QrSymbol {
      // Generation key - the symbol is reused while all of these match.
      std::string text;
      uint32_t    side_px = 0;            // target square side, physical px
      uint32_t    dark    = 0;
      uint32_t    bg      = 0;
      int         ecc     = -1;
      int         quiet   = -1;
      // Rasterised bitmap (BGRA8 premultiplied, square).
      std::vector<uint8_t> pixels;        // w_px * h_px * 4 bytes
      uint32_t    w_px  = 0;
      uint32_t    h_px  = 0;
      float       scale = 1.0f;
      // Per-(ctx) GPU upload. Dropped + re-uploaded on a generation bump
      // (device loss).
      std::unordered_map<neui_render_ctx_t, CtxBmp> bitmaps;
    };

    // The rasterised-symbol cache. A compound asset is shared across many
    // widgets, but the string each encodes is per-widget (its AttrBag), so a
    // single held bitmap would thrash when two widgets show different codes -
    // and only ever display the last-painted one. Instead the layer keeps a
    // small generation-keyed cache: each distinct (text, size, colours, ecc,
    // quiet) combination owns its own buffer + uploads, so N widgets render N
    // different QR codes from one shared layer. FIFO-capped to bound memory.
    // `mutable` - a lazy memoization built during paint (which holds a const
    // CompoundLayer&); it does not change the layer's logical state.
    mutable std::vector<std::unique_ptr<QrSymbol>> qr_cache;

    // Bindings per property name.
    std::unordered_map<std::string, CompoundBinding> bindings;
    // Quantisation per property name, kept apart from the binding so it can be
    // set before one exists and survives a re-bind. Empty unless used.
    std::unordered_map<std::string, float> bind_steps;
  };

  // The compound asset itself - a slot-reused vector of CompoundLayer.
  // The slot id maps to layer.id & 0xffff; the upper 16 bits of a
  // neui_compound_layer_t carry the owning asset's slot (we do NOT
  // encode the session id there - bindings are looked up against the
  // widget's session, not the asset's; the asset already lives in a
  // session-owned AssetManager so cross-session access is filtered at
  // the asset level).
  struct CompoundAsset
  {
    std::vector<std::unique_ptr<CompoundLayer>> layers;
    std::vector<uint32_t>                       free_slots;
  };

  // ---- Handle encoding -----------------------------------------------------

  // neui_compound_layer_t.id = (asset_slot << 16) | layer_slot.
  inline uint32_t compound_layer_asset_slot(neui_compound_layer_t lid)
  { return (lid.id >> 16) & 0xffff; }
  inline uint32_t compound_layer_slot(neui_compound_layer_t lid)
  { return lid.id & 0xffff; }
  inline neui_compound_layer_t pack_compound_layer(uint32_t asset_slot, uint32_t layer_slot)
  { return { ((asset_slot & 0xffff) << 16) | (layer_slot & 0xffff) }; }

  // ---- Layer table management ---------------------------------------------

  // Defined further down (Template parsing section); forward-declared here so
  // compound_add_layer can stamp the QR layer's default "{value}" template.
  inline std::vector<TextSegment> parse_template(const char* src);

  // Defined further down; forward-declared so compound_add_child_layer can
  // validate the parent layer's kind before the accessor's definition.
  inline CompoundLayer* compound_get_layer(CompoundAsset& ca, uint32_t slot);
  inline const CompoundLayer* compound_get_layer(const CompoundAsset& ca, uint32_t slot);

  // parent_slot == 0 -> top-level layer; otherwise the new layer is a child of
  // the GROUP layer at parent_slot (caller is responsible for validating that
  // parent_slot names a GROUP - see compound_add_child_layer).
  inline uint32_t compound_add_layer_parented(CompoundAsset& ca,
                                              neui_compound_layer_kind_t kind,
                                              int z, uint32_t parent_slot)
  {
    auto layer = std::make_unique<CompoundLayer>();
    layer->kind   = kind;
    layer->z      = z;
    layer->parent = parent_slot;
    // QR layers default to encoding the "{value}" attr; a client can override
    // the template via set_string(..., "text", ...) or supply NEUI_ATTR_QRCODE.
    if (kind == NEUI_COMPOUND_LAYER_QR) {
      layer->text_template = "{value}";
      layer->text_segments = parse_template("{value}");
    }
    uint32_t slot;
    if (!ca.free_slots.empty()) {
      slot = ca.free_slots.back();
      ca.free_slots.pop_back();
      ca.layers[slot] = std::move(layer);
    } else {
      if (ca.layers.empty()) ca.layers.emplace_back(nullptr);  // slot 0 unused
      slot = static_cast<uint32_t>(ca.layers.size());
      ca.layers.emplace_back(std::move(layer));
    }
    return slot;
  }

  inline uint32_t compound_add_layer(CompoundAsset& ca,
                                      neui_compound_layer_kind_t kind,
                                      int z)
  {
    return compound_add_layer_parented(ca, kind, z, /*parent_slot*/0);
  }

  // Add a layer inside the GROUP at parent_slot. Returns UINT32_MAX (the host
  // maps this to compound_layer_none) when parent_slot does not name an
  // existing NEUI_COMPOUND_LAYER_GROUP layer of this asset.
  inline uint32_t compound_add_child_layer(CompoundAsset& ca, uint32_t parent_slot,
                                           neui_compound_layer_kind_t kind, int z)
  {
    const CompoundLayer* parent = compound_get_layer(ca, parent_slot);
    if (!parent || parent->kind != NEUI_COMPOUND_LAYER_GROUP) return UINT32_MAX;
    return compound_add_layer_parented(ca, kind, z, parent_slot);
  }

  // Remove a layer. A GROUP layer removes its whole subtree (every descendant)
  // so no child is left pointing at a freed parent slot.
  inline void compound_remove_layer(CompoundAsset& ca, uint32_t slot)
  {
    if (slot == 0 || slot >= ca.layers.size()) return;
    if (!ca.layers[slot]) return;
    // Recurse into children first so nested groups are torn down depth-first.
    for (uint32_t i = 1; i < ca.layers.size(); ++i) {
      if (ca.layers[i] && ca.layers[i]->parent == slot)
        compound_remove_layer(ca, i);
    }
    ca.layers[slot].reset();
    ca.free_slots.push_back(slot);
  }

  inline void compound_clear(CompoundAsset& ca)
  {
    ca.layers.clear();
    ca.free_slots.clear();
  }

  // Destroy the cached GPU upload of every QR layer's internally-held bitmap
  // for `ctx`, invoking `destroy(void* bmp)` per handle. Called by the asset
  // store's release_context / clear so a window's QR-layer uploads are freed
  // alongside the ordinary asset uploads when its render context dies.
  // `destroy` is the backend's destroy_bitmap bound to ctx by the caller.
  template <typename DestroyFn>
  inline void compound_release_ctx_bitmaps(CompoundAsset& ca,
                                           neui_render_ctx_t ctx,
                                           DestroyFn destroy)
  {
    for (auto& layer : ca.layers) {
      if (!layer) continue;
      for (auto& sym : layer->qr_cache) {
        if (!sym) continue;
        auto it = sym->bitmaps.find(ctx);
        if (it == sym->bitmaps.end()) continue;
        if (it->second.bmp) destroy(it->second.bmp);
        sym->bitmaps.erase(it);
      }
    }
  }

  // Upper bound on the per-layer rasterised-symbol cache (distinct QR values
  // a single shared layer keeps live). Oldest entries are evicted FIFO.
  inline constexpr size_t k_qr_cache_max = 16;

  inline CompoundLayer* compound_get_layer(CompoundAsset& ca, uint32_t slot)
  {
    if (slot == 0 || slot >= ca.layers.size()) return nullptr;
    return ca.layers[slot].get();
  }

  inline const CompoundLayer* compound_get_layer(const CompoundAsset& ca, uint32_t slot)
  {
    if (slot == 0 || slot >= ca.layers.size()) return nullptr;
    return ca.layers[slot].get();
  }

  // ---- Template parsing ----------------------------------------------------

  // Parse a template string into a sequence of (literal, key) segments.
  // `{key}` becomes a key segment; `{{` / `}}` collapse to literal `{` / `}`.
  // Malformed input (unclosed `{`, stray `}`) falls through as literal text -
  // never throws, never asserts.
  inline std::vector<TextSegment> parse_template(const char* src)
  {
    std::vector<TextSegment> out;
    if (!src) return out;
    std::string literal_run;
    auto flush_literal = [&]() {
      if (literal_run.empty()) return;
      TextSegment s;
      s.is_literal = true;
      s.text       = std::move(literal_run);
      literal_run.clear();
      out.push_back(std::move(s));
    };
    for (const char* p = src; *p; ) {
      char c = *p;
      if (c == '{') {
        if (p[1] == '{') {
          literal_run.push_back('{');
          p += 2;
          continue;
        }
        // Look for closing '}'. If not found, treat the '{' as literal.
        const char* close = p + 1;
        while (*close && *close != '}') ++close;
        if (*close != '}') {
          literal_run.push_back(c);
          ++p;
          continue;
        }
        flush_literal();
        TextSegment s;
        s.is_literal = false;
        s.text       = std::string(p + 1, close);
        out.push_back(std::move(s));
        p = close + 1;
      } else if (c == '}') {
        if (p[1] == '}') {
          literal_run.push_back('}');
          p += 2;
        } else {
          literal_run.push_back('}');
          ++p;
        }
      } else {
        literal_run.push_back(c);
        ++p;
      }
    }
    flush_literal();
    return out;
  }

  // Render a parsed template against a widget's attribute bag. INT32
  // attrs are formatted as %d, FLOAT attrs as %g, STRING attrs as-is.
  // Missing attrs yield empty.
  inline std::string render_template(const std::vector<TextSegment>& segs,
                                       const AttrBag* bag)
  {
    std::string out;
    char numbuf[32];
    for (const auto& s : segs) {
      if (s.is_literal) { out += s.text; continue; }
      if (!bag) continue;
      // Probe each type. Order: STRING (the most expected for labels),
      // FLOAT, INT32. has() + typed get is the cleanest pattern.
      if (!bag->has(s.text)) continue;
      if (const char* sv = bag->get_string(s.text)) { out += sv; continue; }
      int32_t i = bag->get_int(s.text, 0);
      if (i != 0) { snprintf(numbuf, sizeof(numbuf), "%d", i); out += numbuf; continue; }
      float f = bag->get_float(s.text, 0.0f);
      if (f != 0.0f) { snprintf(numbuf, sizeof(numbuf), "%g", static_cast<double>(f)); out += numbuf; continue; }
      // exact zero - emit "0" (could be int or float; pick int form).
      out += "0";
    }
    return out;
  }

  // ---- Geometry ------------------------------------------------------------

  struct LayerRect { float x, y, w, h; };

  // Resolve the anchor's offset within a (w, h) rect, in pixels relative
  // to that rect's top-left.
  inline void anchor_point(neui_anchor_t a, float w, float h, float& out_x, float& out_y)
  {
    switch (a % 3) {
      case 0: out_x = 0.0f;     break;  // *_LEFT
      case 1: out_x = w * 0.5f; break;  // *_TOP/CENTER/BOTTOM
      case 2: out_x = w;        break;  // *_RIGHT
    }
    switch (a / 3) {
      case 0: out_y = 0.0f;     break;  // TOP_*
      case 1: out_y = h * 0.5f; break;  // (middle row)
      case 2: out_y = h;        break;  // BOTTOM_*
    }
  }

  // Compute the layer's effective rect in widget-local coordinates.
  // `eff_w` / `eff_h` are the layer's resolved size (FILL replaced with
  // parent_w / parent_h, then any binding-driven width/height already
  // applied by the caller before invoking this helper).
  inline LayerRect resolve_layer_rect(float parent_w, float parent_h,
                                        neui_anchor_t parent_anchor,
                                        neui_anchor_t self_anchor,
                                        float offset_x, float offset_y,
                                        float eff_w, float eff_h)
  {
    float pax = 0, pay = 0;
    float sax = 0, say = 0;
    anchor_point(parent_anchor, parent_w, parent_h, pax, pay);
    anchor_point(self_anchor,   eff_w,    eff_h,    sax, say);
    LayerRect r;
    r.x = pax - sax + offset_x;
    r.y = pay - say + offset_y;
    r.w = eff_w;
    r.h = eff_h;
    return r;
  }

  // ---- Bindings + effective values ----------------------------------------

  // Round-to-nearest float-to-int (matches "ties round away from zero",
  // standard lroundf). Used to apply float-typed bindings to int props.
  inline int round_to_int(float f)
  {
    return static_cast<int>(lroundf(f));
  }

  // Evaluate a numeric binding against the widget's attrbag. Returns the
  // post-transform float. Unbound props: caller falls back to the static
  // field value.
  inline float eval_binding_float(const CompoundBinding& b, const AttrBag* bag)
  {
    if (b.is_asset) return 0.0f;
    float x = attr_as_float(bag, b.attr_key, 0.0f);
    // Snap first, then map: `step` is in attribute units, so one step is the
    // same fraction of the range whatever the property it drives is measured in.
    if (b.step > 0.0f) x = std::round(x / b.step) * b.step;
    return b.scale * x + b.offset;
  }

  // Asset bindings: the attribute holds the asset handle's .id as an int.
  inline neui_asset_t eval_binding_asset(const CompoundBinding& b, const AttrBag* bag)
  {
    if (!bag) return asset_none;
    if (!bag->has(b.attr_key)) return asset_none;
    int32_t id = bag->get_int(b.attr_key, 0);
    if (id == 0) {
      // Could also be stored as a FLOAT bit-pattern - unlikely, default to none.
      return asset_none;
    }
    return neui_asset_t{ static_cast<uint32_t>(id) };
  }

  // Effective property readers. They take the layer's static field, the
  // layer's bindings map, and the widget's attrbag, and return the
  // value to use for painting. For int props the float result of a
  // binding is round-to-nearest.

  inline int effective_int(const CompoundLayer& L, const char* prop,
                              int static_value, const AttrBag* bag)
  {
    auto it = L.bindings.find(prop);
    if (it == L.bindings.end()) return static_value;
    return round_to_int(eval_binding_float(it->second, bag));
  }

  inline float effective_float(const CompoundLayer& L, const char* prop,
                                 float static_value, const AttrBag* bag)
  {
    auto it = L.bindings.find(prop);
    if (it == L.bindings.end()) return static_value;
    return eval_binding_float(it->second, bag);
  }

  inline neui_asset_t effective_asset(const CompoundLayer& L, const char* prop,
                                        neui_asset_t static_value,
                                        const AttrBag* bag)
  {
    auto it = L.bindings.find(prop);
    if (it == L.bindings.end()) return static_value;
    return eval_binding_asset(it->second, bag);
  }

  // ---- Property setters (mutator helpers used by the host thunks) ----------

  // Lookup in a small set of known prop names. Unknown names are stored
  // into the layer's bindings map but otherwise inert (callers don't use
  // them for paint; matches AttrBag's "unknown stored" semantics).
  //
  // Returns true if the prop was a known geometry or kind-specific slot
  // and was applied. False results just mean "unrecognised" - not a
  // failure, just ignored.

  inline bool apply_set_int(CompoundLayer& L, const std::string& prop, int v)
  {
    if (prop == "offset_x") { L.offset_x = v; return true; }
    if (prop == "offset_y") { L.offset_y = v; return true; }
    if (prop == "width")    { L.width    = v; return true; }
    if (prop == "height")   { L.height   = v; return true; }
    if (prop == "show_when"){ L.show_when = static_cast<uint32_t>(v); return true; }
    if (L.kind == NEUI_COMPOUND_LAYER_TEXT) {
      if (prop == "color")    {
        L.text_color     = static_cast<uint32_t>(v);
        L.text_color_set = true;
        return true;
      }
      if (prop == "align_x")  { L.text_align_x = v; return true; }
      if (prop == "align_y")  { L.text_align_y = v; return true; }
      if (prop == "weight")   { L.text_weight  = v; return true; }
    }
    if (L.kind == NEUI_COMPOUND_LAYER_RECT || L.kind == NEUI_COMPOUND_LAYER_PATH) {
      if (prop == "fill_color")   { L.fill_color   = static_cast<uint32_t>(v); return true; }
      if (prop == "stroke_color") { L.stroke_color = static_cast<uint32_t>(v); return true; }
      if (prop == "fill_rule")    { L.fill_rule    = (v != 0) ? 1 : 0; return true; }
      if (prop == "stroke_cap")   { L.stroke_cap   = (v < 0) ? 0 : (v > 2 ? 2 : v); return true; }
      if (prop == "stroke_join")  { L.stroke_join  = (v < 0) ? 0 : (v > 2 ? 2 : v); return true; }
    }
    // PATH stroke-trim anchor (§A) - reuses the arc/KNOB polarity enum.
    if (L.kind == NEUI_COMPOUND_LAYER_PATH) {
      if (prop == "polarity")     { L.trim_polarity = (v < 0) ? 0 : (v > 2 ? 2 : v); return true; }
    }
    // RECT value-driven fill (§B) - same polarity enum, plus a fill axis.
    if (L.kind == NEUI_COMPOUND_LAYER_RECT) {
      if (prop == "polarity")     { L.rect_polarity    = (v < 0) ? 0 : (v > 2 ? 2 : v); return true; }
      if (prop == "orientation")  { L.rect_orientation = (v != 0) ? 1 : 0; return true; }
    }
    // ARC reuses fill_color (pie) + stroke_color / stroke_cap (ring) and adds
    // its own polarity / direction enums.
    if (L.kind == NEUI_COMPOUND_LAYER_ARC) {
      if (prop == "fill_color")   { L.fill_color   = static_cast<uint32_t>(v); return true; }
      if (prop == "stroke_color") { L.stroke_color = static_cast<uint32_t>(v); return true; }
      if (prop == "stroke_cap")   { L.stroke_cap   = (v < 0) ? 0 : (v > 2 ? 2 : v); return true; }
      if (prop == "polarity")     { L.arc_polarity = (v < 0) ? 0 : (v > 2 ? 2 : v); return true; }
      if (prop == "direction")    { L.arc_direction = (v != 0) ? 1 : 0; return true; }
    }
    if (L.kind == NEUI_COMPOUND_LAYER_ASSET) {
      if (prop == "tint")  { L.tint  = static_cast<uint32_t>(v); return true; }
      if (prop == "frame") { L.frame = v; return true; }
    }
    if (L.kind == NEUI_COMPOUND_LAYER_QR) {
      if (prop == "fill_color") { L.qr_dark       = static_cast<uint32_t>(v); return true; }
      if (prop == "background") { L.qr_background = static_cast<uint32_t>(v); return true; }
      if (prop == "ecc") {
        L.qr_ecc = (v < 0) ? 0 : (v > 3 ? 3 : v);
        return true;
      }
      if (prop == "quiet_zone") {
        L.qr_quiet = (v < 0) ? 0 : (v > 16 ? 16 : v);
        return true;
      }
    }
    return false;
  }

  inline bool apply_set_float(CompoundLayer& L, const std::string& prop, float v)
  {
    // "<prop>.step" is the quantisation of THAT prop's binding (see
    // eval_binding_float). The compound API has no bind-with-step entry point
    // and this needs no vtable change - the same route stroke_dasharray takes to
    // travel as a string. Kept in `bind_steps` as well as on the binding, so it
    // survives a later (re)bind and does not depend on call order.
    if (prop.size() > 5 && prop.compare(prop.size() - 5, 5, ".step") == 0) {
      std::string target = prop.substr(0, prop.size() - 5);
      float step = (v > 0.0f) ? v : 0.0f;
      L.bind_steps[target] = step;
      auto it = L.bindings.find(target);
      if (it != L.bindings.end()) it->second.step = step;
      return true;
    }
    if (prop == "alpha") {
      if (v < 0.0f) v = 0.0f;
      if (v > 1.0f) v = 1.0f;
      L.alpha = v;
      return true;
    }
    if (L.kind == NEUI_COMPOUND_LAYER_TEXT) {
      if (prop == "size") { L.text_size = v; return true; }
    }
    if (L.kind == NEUI_COMPOUND_LAYER_ASSET) {
      if (prop == "rotation") { L.rotation = v; return true; }
    }
    if (L.kind == NEUI_COMPOUND_LAYER_RECT || L.kind == NEUI_COMPOUND_LAYER_PATH) {
      if (prop == "stroke_width")  { L.stroke_width  = (v < 0.0f) ? 0.0f : v; return true; }
      if (prop == "stroke_miter")  { L.stroke_miter  = (v < 1.0f) ? 4.0f : v; return true; }
      if (prop == "stroke_dash_offset") { L.stroke_dash_offset = v; return true; }
    }
    if (L.kind == NEUI_COMPOUND_LAYER_PATH) {
      if (prop == "value")         { L.trim_value   = v; return true; }   // §A stroke trim
    }
    if (L.kind == NEUI_COMPOUND_LAYER_RECT) {
      if (prop == "corner_radius") { L.corner_radius = (v < 0.0f) ? 0.0f : v; return true; }
      if (prop == "value")         { L.rect_value    = v; return true; }  // §B linear bar
    }
    if (L.kind == NEUI_COMPOUND_LAYER_ARC) {
      if (prop == "stroke_width") { L.stroke_width  = (v < 0.0f) ? 0.0f : v; return true; }
      if (prop == "value")       { L.arc_value     = v; return true; }
      if (prop == "begin_angle") { L.arc_begin_deg = v; return true; }
      if (prop == "end_angle")   { L.arc_end_deg   = v; return true; }
    }
    return false;
  }

  inline bool apply_set_string(CompoundLayer& L, const std::string& prop,
                                 const char* v)
  {
    if (L.kind == NEUI_COMPOUND_LAYER_TEXT && prop == "text") {
      L.text_template = v ? v : "";
      L.text_segments = parse_template(v);
      return true;
    }
    if (L.kind == NEUI_COMPOUND_LAYER_TEXT && prop == "family") {
      L.text_family_template = v ? v : "";
      L.text_family_segments = parse_template(v);
      return true;
    }
    // QR layer reuses the text-template machinery for its "text" prop - the
    // string to encode, with {key} substitution against the widget AttrBag.
    if (L.kind == NEUI_COMPOUND_LAYER_QR && prop == "text") {
      L.text_template = v ? v : "";
      L.text_segments = parse_template(v);
      return true;
    }
    // stroke-dasharray transported as a comma/space-separated string (the
    // compound API has no float-array setter; this avoids a vtable change).
    // Parsed once here into the on/off run-length vector; negatives/zeros drop.
    if ((L.kind == NEUI_COMPOUND_LAYER_PATH || L.kind == NEUI_COMPOUND_LAYER_RECT)
        && prop == "stroke_dasharray") {
      L.stroke_dash.clear();
      for (const char* p = v ? v : ""; *p;) {
        while (*p == ' ' || *p == ',' || *p == '\t') ++p;
        if (!*p) break;
        char* e = nullptr;
        double d = std::strtod(p, &e);
        if (e == p) break;
        p = e;
        if (d > 0.0) L.stroke_dash.push_back(static_cast<float>(d));
      }
      return true;
    }
    return false;
  }

  inline bool apply_set_asset(CompoundLayer& L, const std::string& prop,
                                neui_asset_t v)
  {
    if (L.kind == NEUI_COMPOUND_LAYER_ASSET && prop == "asset") {
      L.asset = v;
      return true;
    }
    return false;
  }

  inline void apply_bind(CompoundLayer& L, const std::string& prop,
                          const char* attr_key, float scale, float offset)
  {
    CompoundBinding b;
    b.attr_key = attr_key ? attr_key : "";
    b.scale    = scale;
    b.offset   = offset;
    b.is_asset = false;
    auto s = L.bind_steps.find(prop);          // set before the bind, or by a re-bind
    if (s != L.bind_steps.end()) b.step = s->second;
    L.bindings[prop] = std::move(b);
  }

  inline void apply_bind_asset(CompoundLayer& L, const std::string& prop,
                                 const char* attr_key)
  {
    CompoundBinding b;
    b.attr_key = attr_key ? attr_key : "";
    b.scale    = 1.0f;
    b.offset   = 0.0f;
    b.is_asset = true;
    L.bindings[prop] = std::move(b);
  }

  inline void apply_unbind(CompoundLayer& L, const std::string& prop)
  {
    L.bindings.erase(prop);
  }

  // Set / clear a RECT or PATH layer's gradient fill. No-op on other kinds
  // (mirrors apply_set_path's "wrong kind = silently ignored"). A null
  // gradient, or one with fewer than two stops, clears any existing
  // gradient so the layer reverts to its solid fill_color. The stop array
  // is copied (the public neui_gradient_t borrows it).
  inline void apply_set_gradient(CompoundLayer& L, const neui_gradient_t* g)
  {
    if (L.kind != NEUI_COMPOUND_LAYER_RECT && L.kind != NEUI_COMPOUND_LAYER_PATH)
      return;
    auto& fg = L.fill_gradient;
    if (!g || !g->stops || g->stop_count < 2) {
      fg.enabled = false;
      fg.stops.clear();
      return;
    }
    fg.enabled = true;
    fg.kind    = g->kind;
    fg.extend  = g->extend;
    fg.start_x = g->start_x; fg.start_y = g->start_y;
    fg.end_x   = g->end_x;   fg.end_y   = g->end_y;
    fg.radius  = g->radius;
    fg.stops.assign(g->stops, g->stops + g->stop_count);
  }

  // Replace the path-layer's command list. No-op on non-PATH layers
  // (mirrors apply_set_string's "wrong kind = silently ignored").
  // Passing NULL or count == 0 clears the path.
  inline void apply_set_path(CompoundLayer& L,
                              const neui_path_cmd_t* cmds, uint32_t count)
  {
    if (L.kind != NEUI_COMPOUND_LAYER_PATH) return;
    L.path_cmds.clear();
    if (!cmds || count == 0) return;
    L.path_cmds.reserve(count);
    for (uint32_t i = 0; i < count; ++i) {
      CompoundLayer::PathCommand c;
      c.kind    = cmds[i].kind;
      c.args[0] = cmds[i].args[0];
      c.args[1] = cmds[i].args[1];
      c.args[2] = cmds[i].args[2];
      c.args[3] = cmds[i].args[3];
      c.args[4] = cmds[i].args[4];
      c.args[5] = cmds[i].args[5];
      L.path_cmds.push_back(c);
    }
  }

  // ---- State filter introspection -----------------------------------------

  // Returns true if any layer has a non-zero show_when filter. Used by the
  // host event paths to decide whether hover / press transitions need to
  // invalidate the widget. Cheap (linear scan of a small layer table).
  inline bool compound_has_state_filters(const CompoundAsset& ca)
  {
    for (const auto& layer : ca.layers) {
      if (layer && layer->show_when != 0u) return true;
    }
    return false;
  }

  // Compose a NEUI_LAYER_STATE_* bitmask from the four input booleans.
  // Always carries exactly one bit per axis (positive or NOT_* form) so
  // the visibility check `(show_when & ~state_mask) == 0` lets a layer
  // require either side of any axis via its show_when bits. `selected` is
  // the widget's logical selected state (from NEUI_ATTR_SELECTED); unlike
  // hovered / pressed it is not host-tracked input but client-owned state.
  inline uint32_t compose_widget_state(bool enabled, bool hovered,
                                       bool pressed, bool selected)
  {
    uint32_t mask = 0;
    mask |= enabled  ? NEUI_LAYER_STATE_ENABLED  : NEUI_LAYER_STATE_NOT_ENABLED;
    mask |= hovered  ? NEUI_LAYER_STATE_HOVERED  : NEUI_LAYER_STATE_NOT_HOVERED;
    mask |= pressed  ? NEUI_LAYER_STATE_PRESSED  : NEUI_LAYER_STATE_NOT_PRESSED;
    mask |= selected ? NEUI_LAYER_STATE_SELECTED : NEUI_LAYER_STATE_NOT_SELECTED;
    return mask;
  }

  // ---- Paint-order iteration -----------------------------------------------

  // Build a stable (z, insertion_order) sort of the compound's layers,
  // skipping nulls. Returns a vector of slot indices. Cheap: ~10s of
  // layers in practice.
  inline std::vector<uint32_t> compound_sorted_slots(const CompoundAsset& ca)
  {
    std::vector<uint32_t> slots;
    slots.reserve(ca.layers.size());
    for (uint32_t i = 1; i < ca.layers.size(); ++i) {
      if (ca.layers[i]) slots.push_back(i);
    }
    std::stable_sort(slots.begin(), slots.end(),
      [&](uint32_t a, uint32_t b) {
        return ca.layers[a]->z < ca.layers[b]->z;
      });
    return slots;
  }

  // Like compound_sorted_slots, but restricted to the direct children of
  // `parent_slot` (parent_slot == 0 -> top-level layers). The paint walk
  // iterates the top level with this, and each GROUP layer iterates its own
  // children with it, so group membership is honoured without flattening.
  inline std::vector<uint32_t> compound_sorted_children(const CompoundAsset& ca,
                                                        uint32_t parent_slot)
  {
    std::vector<uint32_t> slots;
    for (uint32_t i = 1; i < ca.layers.size(); ++i) {
      if (ca.layers[i] && ca.layers[i]->parent == parent_slot) slots.push_back(i);
    }
    std::stable_sort(slots.begin(), slots.end(),
      [&](uint32_t a, uint32_t b) {
        return ca.layers[a]->z < ca.layers[b]->z;
      });
    return slots;
  }

  // Recursion cap on nested GROUP layers (paint-time guard against a malformed
  // / cyclic parent chain). Generous: real layer trees are a handful deep.
  inline constexpr int k_compound_max_group_depth = 16;

} // namespace neui_detail
