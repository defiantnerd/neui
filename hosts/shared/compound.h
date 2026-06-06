#pragma once

#include <algorithm>
#include <cstdint>
#include <cmath>
#include <cstdio>
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
    // Multiplicative ARGB tint applied to the bitmap's pixels. The
    // default 0xFFFFFFFFu is the passthrough sentinel - it short-circuits
    // the tint pipeline and uses the standard untinted bitmap cache. Any
    // other value goes through the per-(asset, ctx, tint) tinted cache
    // (see hosts/shared/painter.h::draw_tinted_bitmap_from_entry).
    uint32_t                 tint     = 0xFFFFFFFFu;

    // Fill / stroke shared between RECT and PATH layers. Both colours
    // default to 0 (fully transparent), so a freshly-added shape with
    // no further setup is invisible - callers opt into a fill, a
    // stroke, or both. corner_radius is rect-only.
    uint32_t                 fill_color    = 0x00000000;
    uint32_t                 stroke_color  = 0x00000000;
    float                    stroke_width  = 0.0f;
    float                    corner_radius = 0.0f;

    // Path-layer geometry. Mirror of the public neui_path_cmd_t layout
    // so set_path can memcpy. Empty vector = no path (layer paints
    // nothing). Replayed in declaration order against the painter's
    // begin_path / move_to / line_to / arc / close_path primitives.
    struct PathCommand {
      uint32_t kind;
      float    args[5];
    };
    std::vector<PathCommand> path_cmds;

    // Bindings per property name.
    std::unordered_map<std::string, CompoundBinding> bindings;
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

  inline uint32_t compound_add_layer(CompoundAsset& ca,
                                      neui_compound_layer_kind_t kind,
                                      int z)
  {
    auto layer = std::make_unique<CompoundLayer>();
    layer->kind = kind;
    layer->z    = z;
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

  inline void compound_remove_layer(CompoundAsset& ca, uint32_t slot)
  {
    if (slot == 0 || slot >= ca.layers.size()) return;
    if (!ca.layers[slot]) return;
    ca.layers[slot].reset();
    ca.free_slots.push_back(slot);
  }

  inline void compound_clear(CompoundAsset& ca)
  {
    ca.layers.clear();
    ca.free_slots.clear();
  }

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
    }
    if (L.kind == NEUI_COMPOUND_LAYER_ASSET) {
      if (prop == "tint") { L.tint = static_cast<uint32_t>(v); return true; }
    }
    return false;
  }

  inline bool apply_set_float(CompoundLayer& L, const std::string& prop, float v)
  {
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
    }
    if (L.kind == NEUI_COMPOUND_LAYER_RECT) {
      if (prop == "corner_radius") { L.corner_radius = (v < 0.0f) ? 0.0f : v; return true; }
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

  // Compose a NEUI_LAYER_STATE_* bitmask from the three input booleans.
  // Always carries exactly one bit per axis (positive or NOT_* form) so
  // the visibility check `(show_when & ~state_mask) == 0` lets a layer
  // require either side of any axis via its show_when bits.
  inline uint32_t compose_widget_state(bool enabled, bool hovered, bool pressed)
  {
    uint32_t mask = 0;
    mask |= enabled ? NEUI_LAYER_STATE_ENABLED : NEUI_LAYER_STATE_NOT_ENABLED;
    mask |= hovered ? NEUI_LAYER_STATE_HOVERED : NEUI_LAYER_STATE_NOT_HOVERED;
    mask |= pressed ? NEUI_LAYER_STATE_PRESSED : NEUI_LAYER_STATE_NOT_PRESSED;
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

} // namespace neui_detail
