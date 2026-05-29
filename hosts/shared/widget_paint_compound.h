#pragma once

#include <neui/neui.h>

#include "compound.h"
#include "attrs.h"
#include "painter.h"
#include "theme_palette.h"

// Host-agnostic paint helper for compound drawables. Both the xpl host
// (CustomDrawWidget::paint / paint_after_children) and the win32 native
// host (paint_customdraw_w32) call into here.
//
// The compound is drawn in two passes:
//   paint_compound_below(p, w, h, bag, ca)  -> z<0 layers
//   paint_compound_above(p, w, h, bag, ca)  -> z>=0 layers
//
// Callers are responsible for the surrounding transform / clip bracket:
//   push_transform; translate(widget.x, widget.y); push_clip(0, 0, w, h)
//     paint_compound_below(...);
//     (descend into child widgets, if any)
//     paint_compound_above(...);
//   pop_clip; pop_transform
// The win32 native host has no "descend into children" step in the
// parent's WM_PAINT (children are independent HWNDs), so it just calls
// _below then _above back-to-back.
//
// `bag` is the widget's own AttrBag (may be null - all binding reads
// then fall back to static field values).

namespace neui_detail
{
  // Resolution helper: parse a layer's geometry against the widget rect.
  // Picks the FILL sentinels up (-1 -> parent dim) and applies bound
  // overrides for offset_x/y/width/height/alpha read against the
  // attribute bag.
  inline LayerRect compute_layer_rect(const CompoundLayer& L,
                                        float parent_w, float parent_h,
                                        const AttrBag* bag)
  {
    int ox = effective_int  (L, "offset_x", L.offset_x, bag);
    int oy = effective_int  (L, "offset_y", L.offset_y, bag);
    int w_static = L.width;
    int h_static = L.height;
    int w_int = effective_int(L, "width",  w_static, bag);
    int h_int = effective_int(L, "height", h_static, bag);
    float eff_w = (w_int == NEUI_COMPOUND_FILL) ? parent_w : static_cast<float>(w_int);
    float eff_h = (h_int == NEUI_COMPOUND_FILL) ? parent_h : static_cast<float>(h_int);
    return resolve_layer_rect(parent_w, parent_h,
                                L.parent_anchor, L.self_anchor,
                                static_cast<float>(ox), static_cast<float>(oy),
                                eff_w, eff_h);
  }

  // Paint a single layer.
  inline void paint_compound_layer(neui_painter_t* p,
                                     const CompoundLayer& L,
                                     float parent_w, float parent_h,
                                     const AttrBag* bag,
                                     uint32_t state_mask)
  {
    if (L.show_when != 0u && (L.show_when & state_mask) == 0u) return;
    float a = effective_float(L, "alpha", L.alpha, bag);
    if (a <= 0.0f) return;
    if (a > 1.0f)  a = 1.0f;

    LayerRect r = compute_layer_rect(L, parent_w, parent_h, bag);
    if (r.w <= 0.0f || r.h <= 0.0f) return;

    bool need_alpha_scope = (a < 1.0f);
    if (need_alpha_scope) k_painter_api.push_alpha(p, a);

    switch (L.kind) {
      case NEUI_COMPOUND_LAYER_TEXT: {
        // Render template against the widget's attrbag.
        std::string text = render_template(L.text_segments, bag);
        if (!text.empty()) {
          float    size  = effective_float(L, "size",  L.text_size,  bag);
          // Color resolution: bind wins over static, static wins over the
          // theme fallback. When the client didn't set "color" and didn't
          // bind it, the layer reads text_primary from the active palette
          // so it stays legible across light / dark themes without the
          // client needing to follow theme changes.
          uint32_t color;
          auto bit = L.bindings.find("color");
          if (bit != L.bindings.end()) {
            color = static_cast<uint32_t>(
              round_to_int(eval_binding_float(bit->second, bag)));
          } else if (L.text_color_set) {
            color = L.text_color;
          } else {
            color = neui_detail::color(neui_detail::ColorRole::text_primary);
          }
          // Font selection: family template (with the same {key} substitution
          // as `text`) + weight. Both default to "system default" / Normal
          // when unset, in which case we skip push_font entirely so backends
          // keep their current behaviour. Family bindings would be unusual
          // (the source is a string), so weight is the bindable one.
          std::string family = render_template(L.text_family_segments, bag);
          int weight = effective_int(L, "weight", L.text_weight, bag);
          bool need_font_scope = (!family.empty() || weight != 0);
          if (need_font_scope)
            k_painter_api.push_font(p,
              family.empty() ? nullptr : family.c_str(), weight);

          // Backends draw_text vertical-centres in the rect and (effectively)
          // left-aligns horizontally by default. For center / right we
          // compute the x offset via measure_text. align_y default = center;
          // align_y top/bottom is approximated by shifting the rect.
          int ax = effective_int(L, "align_x", L.text_align_x, bag);
          float draw_x = r.x;
          float draw_w = r.w;
          if (ax == 1 /* center */ || ax == 2 /* end */) {
            float tw = k_painter_api.measure_text(p, text.c_str(), -1, size);
            if (ax == 1) draw_x = r.x + (r.w - tw) * 0.5f;
            else         draw_x = r.x + (r.w - tw);
            draw_w = tw;
            if (draw_w < 0.0f) draw_w = 0.0f;
          }
          k_painter_api.draw_text(p, draw_x, r.y, draw_w, r.h,
                                    text.c_str(), size, color);

          if (need_font_scope) k_painter_api.pop_font(p);
        }
        break;
      }
      case NEUI_COMPOUND_LAYER_ASSET: {
        neui_asset_t asset = effective_asset(L, "asset", L.asset, bag);
        if (asset.id == asset_none.id) break;
        float rot = effective_float(L, "rotation", L.rotation, bag);

        if (rot != 0.0f) {
          float cx = r.x + r.w * 0.5f;
          float cy = r.y + r.h * 0.5f;
          k_painter_api.push_transform(p);
          k_painter_api.translate(p, cx, cy);
          k_painter_api.rotate(p, rot);
          k_painter_api.translate(p, -cx, -cy);
          k_painter_api.draw_asset(p, asset, r.x, r.y, r.w, r.h);
          k_painter_api.pop_transform(p);
        } else {
          k_painter_api.draw_asset(p, asset, r.x, r.y, r.w, r.h);
        }
        break;
      }
      case NEUI_COMPOUND_LAYER_NONE:
      default:
        break;
    }

    if (need_alpha_scope) k_painter_api.pop_alpha(p);
  }

  // Walk layers in (z, insertion) order, painting those that match the
  // `paint_above_children` filter:
  //   paint_above_children = false -> paint z < 0 layers
  //   paint_above_children = true  -> paint z >= 0 layers
  // The caller is in charge of the widget-local coordinate frame.
  inline void paint_compound_pass(neui_painter_t* p,
                                    const CompoundAsset& ca,
                                    float widget_w, float widget_h,
                                    const AttrBag* bag,
                                    bool paint_above_children,
                                    uint32_t state_mask)
  {
    auto slots = compound_sorted_slots(ca);
    for (uint32_t slot : slots) {
      const CompoundLayer* L = compound_get_layer(ca, slot);
      if (!L) continue;
      bool is_above = (L->z >= 0);
      if (is_above != paint_above_children) continue;
      paint_compound_layer(p, *L, widget_w, widget_h, bag, state_mask);
    }
  }

  // state_mask is a NEUI_LAYER_STATE_* bitmask describing the widget's
  // current state (compose with compose_widget_state). Pass
  // NEUI_LAYER_STATE_ENABLED for callers that don't care about state
  // filtering - layers with show_when == 0 (the default) are visible in
  // every state.
  inline void paint_compound_below(neui_painter_t* p,
                                     const CompoundAsset& ca,
                                     float widget_w, float widget_h,
                                     const AttrBag* bag,
                                     uint32_t state_mask)
  {
    paint_compound_pass(p, ca, widget_w, widget_h, bag, /*above*/false, state_mask);
  }

  inline void paint_compound_above(neui_painter_t* p,
                                     const CompoundAsset& ca,
                                     float widget_w, float widget_h,
                                     const AttrBag* bag,
                                     uint32_t state_mask)
  {
    paint_compound_pass(p, ca, widget_w, widget_h, bag, /*above*/true, state_mask);
  }

} // namespace neui_detail
