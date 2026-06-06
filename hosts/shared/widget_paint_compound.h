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
  // Build a rounded-rectangle path on the painter's path API. Traversal is
  // clockwise in screen space (Y-down), so the four quarter-arcs sweep:
  //   top-left:     pi      -> 1.5*pi   (left  -> top)
  //   top-right:    1.5*pi  -> 2*pi     (top   -> right)
  //   bottom-right: 0       -> 0.5*pi   (right -> bottom)
  //   bottom-left:  0.5*pi  -> pi       (bottom -> left)
  // Caller is responsible for begin_path before and a single
  // fill_path / stroke_path after (path persists across both on D2D + CG).
  inline void build_rounded_rect_path(neui_painter_t* p,
                                        float x, float y,
                                        float w, float h,
                                        float r)
  {
    const float PI = 3.14159265358979323846f;
    float max_r = (w < h ? w : h) * 0.5f;
    if (r > max_r) r = max_r;
    if (r < 0.0f)  r = 0.0f;

    k_painter_api.begin_path(p);
    if (r <= 0.0f) {
      k_painter_api.move_to(p, x,     y);
      k_painter_api.line_to(p, x + w, y);
      k_painter_api.line_to(p, x + w, y + h);
      k_painter_api.line_to(p, x,     y + h);
      k_painter_api.close_path(p);
      return;
    }

    k_painter_api.move_to(p, x,     y + r);
    k_painter_api.arc    (p, x + r,     y + r,     r, PI,           1.5f * PI);
    k_painter_api.line_to(p, x + w - r, y);
    k_painter_api.arc    (p, x + w - r, y + r,     r, 1.5f * PI,    2.0f * PI);
    k_painter_api.line_to(p, x + w,     y + h - r);
    k_painter_api.arc    (p, x + w - r, y + h - r, r, 0.0f,         0.5f * PI);
    k_painter_api.line_to(p, x + r,     y + h);
    k_painter_api.arc    (p, x + r,     y + h - r, r, 0.5f * PI,    PI);
    k_painter_api.close_path(p);
  }

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
    // AND-filter: every bit set in show_when must be present in state_mask.
    // show_when == 0 (default) always passes.
    if ((L.show_when & ~state_mask) != 0u) return;
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
        uint32_t tint = static_cast<uint32_t>(
          effective_int(L, "tint", static_cast<int>(L.tint), bag));
        // tint == 0 (alpha 0) would make the layer invisible; skip the
        // upload/draw entirely rather than running the backend's tint
        // primitive only to output fully-transparent pixels. The
        // 0xFFFFFFFFu passthrough sentinel and any other tint value
        // both route through the same painter_draw_asset_tinted helper;
        // the backend short-circuits effect setup on the passthrough.
        if (tint == 0u) break;

        if (rot != 0.0f) {
          float cx = r.x + r.w * 0.5f;
          float cy = r.y + r.h * 0.5f;
          k_painter_api.push_transform(p);
          k_painter_api.translate(p, cx, cy);
          k_painter_api.rotate(p, rot);
          k_painter_api.translate(p, -cx, -cy);
          painter_draw_asset_tinted(p, asset, r.x, r.y, r.w, r.h, tint);
          k_painter_api.pop_transform(p);
        } else {
          painter_draw_asset_tinted(p, asset, r.x, r.y, r.w, r.h, tint);
        }
        break;
      }
      case NEUI_COMPOUND_LAYER_PATH: {
        if (L.path_cmds.empty()) break;
        uint32_t fill   = static_cast<uint32_t>(
          effective_int  (L, "fill_color",   static_cast<int>(L.fill_color),   bag));
        uint32_t stroke = static_cast<uint32_t>(
          effective_int  (L, "stroke_color", static_cast<int>(L.stroke_color), bag));
        float    sw     = effective_float(L, "stroke_width",  L.stroke_width,  bag);
        if (sw < 0.0f) sw = 0.0f;

        bool has_fill   = ((fill   >> 24) & 0xffu) != 0u;
        bool has_stroke = sw > 0.0f && ((stroke >> 24) & 0xffu) != 0u;
        if (!has_fill && !has_stroke) break;

        // Replay path commands in layer-local space - push a transform so
        // the path's (0, 0) lands at the layer rect's top-left. The
        // outer widget-bounds clip set up by the caller still applies.
        k_painter_api.push_transform(p);
        k_painter_api.translate(p, r.x, r.y);
        k_painter_api.begin_path(p);
        for (const auto& cmd : L.path_cmds) {
          switch (cmd.kind) {
            case NEUI_PATH_CMD_MOVE_TO:
              k_painter_api.move_to(p, cmd.args[0], cmd.args[1]);
              break;
            case NEUI_PATH_CMD_LINE_TO:
              k_painter_api.line_to(p, cmd.args[0], cmd.args[1]);
              break;
            case NEUI_PATH_CMD_ARC:
              k_painter_api.arc(p, cmd.args[0], cmd.args[1], cmd.args[2],
                                  cmd.args[3], cmd.args[4]);
              break;
            case NEUI_PATH_CMD_CLOSE:
              k_painter_api.close_path(p);
              break;
            default:
              break;  // unknown kind, skip
          }
        }
        if (has_fill)   k_painter_api.fill_path  (p, fill);
        if (has_stroke) k_painter_api.stroke_path(p, sw, stroke);
        k_painter_api.pop_transform(p);
        break;
      }
      case NEUI_COMPOUND_LAYER_RECT: {
        uint32_t fill   = static_cast<uint32_t>(
          effective_int  (L, "fill_color",   static_cast<int>(L.fill_color),   bag));
        uint32_t stroke = static_cast<uint32_t>(
          effective_int  (L, "stroke_color", static_cast<int>(L.stroke_color), bag));
        float    sw     = effective_float(L, "stroke_width",  L.stroke_width,  bag);
        float    radius = effective_float(L, "corner_radius", L.corner_radius, bag);
        if (sw < 0.0f) sw = 0.0f;
        if (radius < 0.0f) radius = 0.0f;

        bool has_fill   = ((fill   >> 24) & 0xffu) != 0u;
        bool has_stroke = sw > 0.0f && ((stroke >> 24) & 0xffu) != 0u;
        if (!has_fill && !has_stroke) break;

        if (radius <= 0.0f) {
          if (has_fill)   k_painter_api.fill_rect(p, r.x, r.y, r.w, r.h, fill);
          if (has_stroke) k_painter_api.draw_rect(p, r.x, r.y, r.w, r.h, sw, stroke);
        } else {
          build_rounded_rect_path(p, r.x, r.y, r.w, r.h, radius);
          if (has_fill)   k_painter_api.fill_path  (p, fill);
          if (has_stroke) k_painter_api.stroke_path(p, sw, stroke);
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
