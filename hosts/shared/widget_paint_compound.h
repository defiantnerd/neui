#pragma once

#include <neui/neui.h>

#include <qrcodegen.hpp>

#include "compound.h"
#include "attrs.h"
#include "painter.h"
#include "theme_palette.h"
#include "behavior_runtime.h"  // BEHAVIOR_PI / behavior_clamp (shared math)

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
  // build_rounded_rect_path + append_elliptical_arc now live in painter.h -
  // they back the painter's public fill/draw_round_rect + ellipse entries, and
  // the compound layers below use them from there (same namespace, and
  // painter.h is included above).

  // Arc-length stroke trim for the PATH layer (§A). Flattens the layer's
  // command list into polyline vertices (each carrying a `move` flag = start
  // of a new subpath, no segment leads into it; CLOSE re-adds the closing
  // segment back to the subpath start), measures cumulative arc length, then
  // returns the sub-path covering the fractional span [a, b] of total length as
  // a fresh MOVE_TO / LINE_TO list. Lines are exact; curves + arcs are sampled
  // (dense enough that a stroked trim reads smooth at typical sizes). Boundary
  // segments are split by linear interpolation along the flattened polyline.
  // 0 <= a <= b <= 1 expected; an empty result means nothing to draw. All
  // backend-agnostic math - no D2D / CG / Cairo work.
  inline std::vector<CompoundLayer::PathCommand>
  trim_path_commands(const std::vector<CompoundLayer::PathCommand>& cmds,
                     float a, float b)
  {
    struct FP { float x, y; bool move; };
    std::vector<FP> pts;
    pts.reserve(cmds.size() * 4 + 4);

    const float PI = 3.14159265358979323846f;
    float cx = 0.0f, cy = 0.0f;   // current point
    float sx = 0.0f, sy = 0.0f;   // subpath start
    bool  have_cur = false;
    auto emit = [&](float x, float y, bool mv) { pts.push_back({ x, y, mv }); };

    for (const auto& c : cmds) {
      switch (c.kind) {
        case NEUI_PATH_CMD_MOVE_TO:
          cx = c.args[0]; cy = c.args[1]; sx = cx; sy = cy; have_cur = true;
          emit(cx, cy, true);
          break;
        case NEUI_PATH_CMD_LINE_TO:
          if (!have_cur) {
            cx = c.args[0]; cy = c.args[1]; sx = cx; sy = cy; have_cur = true;
            emit(cx, cy, true);
          } else {
            cx = c.args[0]; cy = c.args[1];
            emit(cx, cy, false);
          }
          break;
        case NEUI_PATH_CMD_CUBIC_TO: {
          if (!have_cur) break;
          const int N = 24;
          const float x0 = cx, y0 = cy;
          const float x1 = c.args[0], y1 = c.args[1];
          const float x2 = c.args[2], y2 = c.args[3];
          const float x3 = c.args[4], y3 = c.args[5];
          for (int i = 1; i <= N; ++i) {
            const float t = static_cast<float>(i) / N, u = 1.0f - t;
            emit(u*u*u*x0 + 3*u*u*t*x1 + 3*u*t*t*x2 + t*t*t*x3,
                 u*u*u*y0 + 3*u*u*t*y1 + 3*u*t*t*y2 + t*t*t*y3, false);
          }
          cx = x3; cy = y3;
          break;
        }
        case NEUI_PATH_CMD_QUAD_TO: {
          if (!have_cur) break;
          const int N = 18;
          const float x0 = cx, y0 = cy;
          const float x1 = c.args[0], y1 = c.args[1];
          const float x2 = c.args[2], y2 = c.args[3];
          for (int i = 1; i <= N; ++i) {
            const float t = static_cast<float>(i) / N, u = 1.0f - t;
            emit(u*u*x0 + 2*u*t*x1 + t*t*x2,
                 u*u*y0 + 2*u*t*y1 + t*t*y2, false);
          }
          cx = x2; cy = y2;
          break;
        }
        case NEUI_PATH_CMD_ARC: {
          const float acx = c.args[0], acy = c.args[1], rr = c.args[2];
          const float a0 = c.args[3], a1 = c.args[4];
          // Canvas arc() convention: connect the current point to the arc start
          // with a line, then sweep. With no current point the start opens a
          // new subpath instead.
          const float startx = acx + rr * std::cos(a0);
          const float starty = acy + rr * std::sin(a0);
          if (have_cur) emit(startx, starty, false);
          else { emit(startx, starty, true); sx = startx; sy = starty; }
          int N = static_cast<int>(std::ceil(std::fabs(a1 - a0) / (PI / 24.0f)));
          if (N < 1) N = 1;
          for (int i = 1; i <= N; ++i) {
            const float t = a0 + (a1 - a0) * (static_cast<float>(i) / N);
            emit(acx + rr * std::cos(t), acy + rr * std::sin(t), false);
          }
          cx = acx + rr * std::cos(a1);
          cy = acy + rr * std::sin(a1);
          have_cur = true;
          break;
        }
        case NEUI_PATH_CMD_CLOSE:
          if (have_cur) { emit(sx, sy, false); cx = sx; cy = sy; }
          break;
        default:
          break;
      }
    }

    std::vector<CompoundLayer::PathCommand> out;
    if (pts.size() < 2) return out;

    // Cumulative arc length per vertex (a `move` vertex adds no segment).
    std::vector<float> cum(pts.size(), 0.0f);
    for (size_t i = 1; i < pts.size(); ++i) {
      if (pts[i].move) { cum[i] = cum[i - 1]; continue; }
      const float dx = pts[i].x - pts[i - 1].x;
      const float dy = pts[i].y - pts[i - 1].y;
      cum[i] = cum[i - 1] + std::sqrt(dx * dx + dy * dy);
    }
    const float total = cum.back();
    if (total <= 0.0f) return out;

    const float la = a * total, lb = b * total;
    if (lb <= la) return out;

    auto push = [&](uint32_t kind, float x, float y) {
      CompoundLayer::PathCommand pc{};
      pc.kind = kind; pc.args[0] = x; pc.args[1] = y;
      out.push_back(pc);
    };

    bool pen_down = false;
    for (size_t i = 1; i < pts.size(); ++i) {
      if (pts[i].move) { pen_down = false; continue; }  // subpath break
      const float c0 = cum[i - 1], c1 = cum[i];
      if (c1 <= c0) continue;                            // zero-length segment
      float ts = (la - c0) / (c1 - c0);
      float te = (lb - c0) / (c1 - c0);
      if (ts < 0.0f) ts = 0.0f; else if (ts > 1.0f) ts = 1.0f;
      if (te < 0.0f) te = 0.0f; else if (te > 1.0f) te = 1.0f;
      if (te <= ts) continue;                            // segment outside span
      const float ax = pts[i - 1].x, ay = pts[i - 1].y;
      const float bx = pts[i].x,     by = pts[i].y;
      if (!pen_down) {
        push(NEUI_PATH_CMD_MOVE_TO, ax + (bx - ax) * ts, ay + (by - ay) * ts);
        pen_down = true;
      }
      push(NEUI_PATH_CMD_LINE_TO, ax + (bx - ax) * te, ay + (by - ay) * te);
    }
    return out;
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

  // Map a layer's stored (normalised) gradient onto an absolute neui_gradient_t
  // for a given draw origin + size. RECT fills draw in widget-local space with
  // no transform, so pass the layer rect's origin; PATH fills draw under a
  // translate(r.x, r.y), so pass origin (0, 0). `radius` (radial) is taken as
  // a fraction of the larger dimension. The returned gradient borrows the
  // layer's stop vector (stable for the paint call).
  inline neui_gradient_t resolve_layer_gradient(
      const CompoundLayer::GradientFill& g,
      float ox, float oy, float w, float h)
  {
    neui_gradient_t out{};
    out.kind       = g.kind;
    out.stops      = g.stops.data();
    out.stop_count = static_cast<uint32_t>(g.stops.size());
    out.extend     = g.extend;
    out.start_x = ox + g.start_x * w;
    out.start_y = oy + g.start_y * h;
    out.end_x   = ox + g.end_x   * w;
    out.end_y   = oy + g.end_y   * h;
    const float maxdim = (w > h) ? w : h;
    out.radius  = g.radius * maxdim;
    return out;
  }

  // ---- QR layer -----------------------------------------------------------

  // Resolve a QR layer's source string: NEUI_ATTR_QRCODE on the widget's
  // AttrBag (if present + non-empty) wins; otherwise the layer's "text"
  // template (default "{value}") rendered against the bag.
  inline std::string qr_resolve_text(const CompoundLayer& L, const AttrBag* bag)
  {
    if (bag && bag->has(NEUI_ATTR_QRCODE)) {
      if (const char* s = bag->get_string(NEUI_ATTR_QRCODE)) {
        if (s[0] != '\0') return std::string(s);
      }
    }
    return render_template(L.text_segments, bag);
  }

  // Premultiply a straight-alpha 0xAARRGGBB into a 4-byte BGRA8 sample.
  inline void qr_premul_bgra(uint32_t argb, uint8_t out[4])
  {
    const uint32_t a = (argb >> 24) & 0xffu;
    const uint32_t r = (argb >> 16) & 0xffu;
    const uint32_t g = (argb >> 8)  & 0xffu;
    const uint32_t b =  argb        & 0xffu;
    out[0] = static_cast<uint8_t>(b * a / 255u);
    out[1] = static_cast<uint8_t>(g * a / 255u);
    out[2] = static_cast<uint8_t>(r * a / 255u);
    out[3] = static_cast<uint8_t>(a);
  }

  // Rasterise the QR symbol into `sym`'s BGRA8 buffer. `sym.side_px` is the
  // target square side in physical pixels; module size is the largest integer
  // that fits the symbol + quiet zone inside it, so the bitmap is crisp (no
  // fractional-module blur). On encode failure (text too long for any version)
  // or empty text, the buffer is left empty (the layer draws nothing).
  inline void qr_rasterize(CompoundLayer::QrSymbol& sym, float scale)
  {
    sym.pixels.clear();
    sym.w_px = sym.h_px = 0;
    sym.scale = (scale > 0.0f) ? scale : 1.0f;
    if (sym.text.empty() || sym.side_px == 0u) return;
    try {
      qrcodegen::QrCode qr = qrcodegen::QrCode::encodeText(
        sym.text.c_str(), static_cast<qrcodegen::QrCode::Ecc>(sym.ecc));
      const int      N = qr.getSize();
      const int      quiet = sym.quiet;
      const uint32_t T = static_cast<uint32_t>(N) + 2u * static_cast<uint32_t>(quiet);
      uint32_t module_px = (T != 0u) ? (sym.side_px / T) : 0u;
      if (module_px == 0u) module_px = 1u;
      const uint32_t dim = module_px * T;

      uint8_t dpx[4], bpx[4];
      qr_premul_bgra(sym.dark, dpx);
      qr_premul_bgra(sym.bg,   bpx);

      sym.pixels.resize(static_cast<size_t>(dim) * dim * 4u);
      for (uint32_t y = 0; y < dim; ++y) {
        const int my = static_cast<int>(y / module_px) - quiet;
        uint8_t* row = sym.pixels.data() + static_cast<size_t>(y) * dim * 4u;
        for (uint32_t x = 0; x < dim; ++x) {
          const int mx = static_cast<int>(x / module_px) - quiet;
          const bool darkmod =
            (mx >= 0 && mx < N && my >= 0 && my < N) && qr.getModule(mx, my);
          const uint8_t* src = darkmod ? dpx : bpx;
          uint8_t* px = row + static_cast<size_t>(x) * 4u;
          px[0] = src[0]; px[1] = src[1]; px[2] = src[2]; px[3] = src[3];
        }
      }
      sym.w_px = dim;
      sym.h_px = dim;
    } catch (...) {
      // data_too_long or any other failure: leave the buffer empty.
      sym.pixels.clear();
      sym.w_px = sym.h_px = 0;
    }
  }

  // Find the cached symbol matching the generation key, or rasterise a new one
  // (FIFO-evicting the oldest when the cache is full). Returns nullptr only if
  // the freshly-built symbol failed to rasterise (empty / too-long text).
  // Because the cache is keyed by the full key (incl. the resolved text), one
  // shared layer serves many widgets each showing a different QR code.
  inline CompoundLayer::QrSymbol* qr_get_symbol(
      const CompoundLayer& L, const std::string& text, uint32_t side_px,
      uint32_t dark, uint32_t bg, int ecc, int quiet, float scale)
  {
    for (auto& sym : L.qr_cache) {
      if (sym && sym->text == text && sym->side_px == side_px &&
          sym->dark == dark && sym->bg == bg &&
          sym->ecc == ecc && sym->quiet == quiet)
        return sym.get();
    }
    auto sym = std::make_unique<CompoundLayer::QrSymbol>();
    sym->text = text; sym->side_px = side_px; sym->dark = dark; sym->bg = bg;
    sym->ecc = ecc;   sym->quiet = quiet;
    qr_rasterize(*sym, scale);
    if (sym->pixels.empty()) return nullptr;
    if (L.qr_cache.size() >= k_qr_cache_max)
      L.qr_cache.erase(L.qr_cache.begin());  // FIFO evict oldest
    L.qr_cache.push_back(std::move(sym));
    return L.qr_cache.back().get();
  }

  // Lazy per-(ctx) GPU upload of a cached symbol's bitmap. Mirrors
  // upload_entry_bitmap; re-uploads on a backend generation bump (device loss).
  inline void* qr_upload(neui_painter_t* p, CompoundLayer::QrSymbol& sym)
  {
    neui_render_backend_t* backend = p ? p->backend : nullptr;
    neui_render_ctx_t      ctx     = p ? p->ctx : nullptr;
    if (!backend || !ctx || sym.pixels.empty()) return nullptr;
    const uint32_t gen = backend->get_context_generation
      ? backend->get_context_generation(ctx) : 0u;
    auto it = sym.bitmaps.find(ctx);
    if (it != sym.bitmaps.end() && it->second.generation != gen) {
      if (backend->destroy_bitmap && it->second.bmp)
        backend->destroy_bitmap(ctx, it->second.bmp);
      sym.bitmaps.erase(it);
      it = sym.bitmaps.end();
    }
    if (it == sym.bitmaps.end()) {
      if (!backend->create_bitmap) return nullptr;
      void* bmp = backend->create_bitmap(ctx, sym.w_px, sym.h_px,
                                         sym.pixels.data(), sym.scale);
      if (!bmp) return nullptr;
      it = sym.bitmaps.emplace(ctx, CompoundLayer::CtxBmp{ bmp, gen }).first;
    }
    return it->second.bmp;
  }

  // Paint a single layer. `ca` + `self_slot` let a GROUP layer find and paint
  // its children; `depth` guards against a malformed / cyclic parent chain.
  inline void paint_compound_layer(neui_painter_t* p,
                                     const CompoundAsset& ca,
                                     uint32_t self_slot,
                                     const CompoundLayer& L,
                                     float parent_w, float parent_h,
                                     const AttrBag* bag,
                                     uint32_t state_mask,
                                     int depth)
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
        // Filmstrip cell. Default 0; commonly bound to a value attr. On an
        // ordinary (untagged) asset the frame is ignored and the whole bitmap
        // draws, so routing every asset layer through the frame-aware helper
        // is behaviour-preserving for non-filmstrip layers.
        int frame_i = effective_int(L, "frame", L.frame, bag);
        uint32_t frame = (frame_i < 0) ? 0u : static_cast<uint32_t>(frame_i);
        // tint == 0 (alpha 0) would make the layer invisible; skip the
        // upload/draw entirely rather than running the backend's tint
        // primitive only to output fully-transparent pixels. The
        // 0xFFFFFFFFu passthrough sentinel and any other tint value
        // both route through the same painter_draw_asset_frame_tinted helper;
        // the backend short-circuits effect setup on the passthrough.
        if (tint == 0u) break;

        if (rot != 0.0f) {
          float cx = r.x + r.w * 0.5f;
          float cy = r.y + r.h * 0.5f;
          k_painter_api.push_transform(p);
          k_painter_api.translate(p, cx, cy);
          k_painter_api.rotate(p, rot);
          k_painter_api.translate(p, -cx, -cy);
          painter_draw_asset_frame_tinted(p, asset, frame, r.x, r.y, r.w, r.h, tint);
          k_painter_api.pop_transform(p);
        } else {
          painter_draw_asset_frame_tinted(p, asset, frame, r.x, r.y, r.w, r.h, tint);
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

        bool grad_fill   = L.fill_gradient.enabled && L.fill_gradient.stops.size() >= 2;
        bool solid_fill  = ((fill   >> 24) & 0xffu) != 0u;
        bool has_fill    = grad_fill || solid_fill;
        bool has_stroke  = sw > 0.0f && ((stroke >> 24) & 0xffu) != 0u;
        if (!has_fill && !has_stroke) break;

        // Value-driven stroke trim (§A). `value` is a POSITION along the path;
        // polarity picks the anchor (min = 0 / start, center = 0.5, max = 1 /
        // end) and the stroked sub-path spans anchor -> value, matching the ARC
        // and RECT layers. min: [0,v] (default v=1 ⇒ whole path, byte-for-byte);
        // center: [min(.5,v), max(.5,v)]; max: [v,1]. An empty span (anchor==v)
        // strokes nothing. Fills are never trimmed - a partial fill of an
        // arbitrary path has no canonical meaning, so the fill uses the whole path.
        float trim_v = behavior_clamp(
          effective_float(L, "value", L.trim_value, bag), 0.0f, 1.0f);
        int   trim_pol = effective_int(L, "polarity", L.trim_polarity, bag);
        float t_anchor = (trim_pol == 1) ? 0.5f : (trim_pol == 2 ? 1.0f : 0.0f);
        float ta = std::fmin(t_anchor, trim_v), tb = std::fmax(t_anchor, trim_v);
        bool  full_path = ta <= 0.0f && tb >= 1.0f;   // whole path: skip trimming
        bool  do_trim   = has_stroke && !full_path;

        std::vector<CompoundLayer::PathCommand> trimmed;
        if (do_trim && tb > ta) {
          trimmed = trim_path_commands(L.path_cmds, ta, tb);
        }

        // Replay path commands in layer-local space - push a transform so
        // the path's (0, 0) lands at the layer rect's top-left. The
        // outer widget-bounds clip set up by the caller still applies.
        k_painter_api.push_transform(p);
        k_painter_api.translate(p, r.x, r.y);

        auto replay = [&](const std::vector<CompoundLayer::PathCommand>& list) {
          k_painter_api.begin_path(p);
          // fill-rule resets to NONZERO on begin_path, so set it before any verb.
          if (L.fill_rule != 0 && k_painter_api.set_fill_rule)
            k_painter_api.set_fill_rule(p, NEUI_FILL_RULE_EVENODD);
          for (const auto& cmd : list) {
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
              case NEUI_PATH_CMD_CUBIC_TO:
                k_painter_api.cubic_to(p, cmd.args[0], cmd.args[1], cmd.args[2],
                                          cmd.args[3], cmd.args[4], cmd.args[5]);
                break;
              case NEUI_PATH_CMD_QUAD_TO:
                k_painter_api.quad_to(p, cmd.args[0], cmd.args[1], cmd.args[2], cmd.args[3]);
                break;
              case NEUI_PATH_CMD_CLOSE:
                k_painter_api.close_path(p);
                break;
              default:
                break;  // unknown kind, skip
            }
          }
        };
        auto do_fill = [&]() {
          // Path is in the translated frame, so the gradient is too: origin (0,0).
          if (grad_fill) {
            neui_gradient_t g = resolve_layer_gradient(L.fill_gradient, 0.0f, 0.0f, r.w, r.h);
            k_painter_api.fill_path_gradient(p, &g);
          } else if (solid_fill) {
            k_painter_api.fill_path(p, fill);
          }
        };
        auto do_stroke = [&]() {
          bool styled = L.stroke_cap != 0 || L.stroke_join != 0 ||
                        !L.stroke_dash.empty() || L.stroke_miter != 4.0f;
          if (styled && k_painter_api.stroke_path_styled) {
            neui_stroke_style_t style{};
            style.cap         = static_cast<neui_line_cap_t>(L.stroke_cap);
            style.join        = static_cast<neui_line_join_t>(L.stroke_join);
            style.miter_limit = L.stroke_miter;
            style.dash_array  = L.stroke_dash.empty() ? nullptr : L.stroke_dash.data();
            style.dash_count  = static_cast<uint32_t>(L.stroke_dash.size());
            style.dash_offset = L.stroke_dash_offset;
            k_painter_api.stroke_path_styled(p, sw, stroke, &style);
          } else {
            k_painter_api.stroke_path(p, sw, stroke);
          }
        };

        if (!do_trim) {
          // No trim: one replay carries both the fill and the stroke (the
          // path persists across both on D2D + CG), exactly as before.
          replay(L.path_cmds);
          if (has_fill)   do_fill();
          if (has_stroke) do_stroke();
        } else {
          // Trimming: the fill (if any) still uses the whole path; the stroke
          // replays the trimmed sub-path. An empty span leaves `trimmed` empty,
          // so the fill (track) still shows while nothing is stroked.
          if (has_fill) { replay(L.path_cmds); do_fill(); }
          if (!trimmed.empty()) { replay(trimmed); do_stroke(); }
        }
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

        bool grad_fill   = L.fill_gradient.enabled && L.fill_gradient.stops.size() >= 2;
        bool solid_fill  = ((fill   >> 24) & 0xffu) != 0u;
        bool has_fill    = grad_fill || solid_fill;
        bool has_stroke  = sw > 0.0f && ((stroke >> 24) & 0xffu) != 0u;
        if (!has_fill && !has_stroke) break;

        // Value-driven fill (§B - the linear bar). `value` is the painted
        // fraction of the rect along the fill axis; default 1 ⇒ the sub-rect
        // equals the full rect, so an unbound RECT paints exactly as before.
        // The stroke always outlines the FULL rect as a track; the gradient
        // stays mapped to the full rect so it reads as a fixed scale revealed
        // by the fill.
        float rv  = behavior_clamp(effective_float(L, "value", L.rect_value, bag),
                                   0.0f, 1.0f);
        int   ori = effective_int(L, "orientation", L.rect_orientation, bag);
        int   pol = effective_int(L, "polarity",    L.rect_polarity,    bag);
        // `value` is a POSITION along the fill axis; polarity picks the anchor
        // (min = 0 / origin, center = 0.5, max = 1 / far end) and the fill spans
        // anchor -> value, matching the ARC layer exactly. min: [0,v] (grows
        // from the origin); center: [min(.5,v), max(.5,v)] (pan/balance from the
        // middle); max: [v,1] (anchored at the far end, empty at v=1).
        float anchor = (pol == 1) ? 0.5f : (pol == 2 ? 1.0f : 0.0f);
        float lo = std::fmin(anchor, rv), hi = std::fmax(anchor, rv);
        float fx = r.x, fy = r.y, fw = r.w, fh = r.h;
        if (ori == 1) {              // vertical: fill along y
          fy = r.y + lo * r.h;  fh = (hi - lo) * r.h;
        } else {                     // horizontal: fill along x
          fx = r.x + lo * r.w;  fw = (hi - lo) * r.w;
        }
        bool draw_fill = has_fill && fw > 0.0f && fh > 0.0f;

        if (radius <= 0.0f) {
          // RECT fills draw with no transform pushed, so the gradient is in
          // absolute widget-local space: origin = the layer rect's top-left.
          if (draw_fill) {
            if (grad_fill) {
              neui_gradient_t g = resolve_layer_gradient(L.fill_gradient, r.x, r.y, r.w, r.h);
              k_painter_api.fill_rect_gradient(p, fx, fy, fw, fh, &g);
            } else if (solid_fill) {
              k_painter_api.fill_rect(p, fx, fy, fw, fh, fill);
            }
          }
          if (has_stroke) k_painter_api.draw_rect(p, r.x, r.y, r.w, r.h, sw, stroke);
        } else {
          if (draw_fill) {
            build_rounded_rect_path(p, fx, fy, fw, fh, radius);
            if (grad_fill) {
              neui_gradient_t g = resolve_layer_gradient(L.fill_gradient, r.x, r.y, r.w, r.h);
              k_painter_api.fill_path_gradient(p, &g);
            } else if (solid_fill) {
              k_painter_api.fill_path(p, fill);
            }
          }
          if (has_stroke) {
            build_rounded_rect_path(p, r.x, r.y, r.w, r.h, radius);
            k_painter_api.stroke_path(p, sw, stroke);
          }
        }
        break;
      }
      case NEUI_COMPOUND_LAYER_ARC: {
        uint32_t fill   = static_cast<uint32_t>(
          effective_int  (L, "fill_color",   static_cast<int>(L.fill_color),   bag));
        uint32_t stroke = static_cast<uint32_t>(
          effective_int  (L, "stroke_color", static_cast<int>(L.stroke_color), bag));
        float    sw     = effective_float(L, "stroke_width", L.stroke_width, bag);
        if (sw < 0.0f) sw = 0.0f;
        bool has_fill   = ((fill   >> 24) & 0xffu) != 0u;
        bool has_stroke = sw > 0.0f && ((stroke >> 24) & 0xffu) != 0u;
        if (!has_fill && !has_stroke) break;

        // Sweep parameters. `value` is the painted fraction of [begin, end];
        // begin/end are in degrees (0 = 12 o'clock, +cw); polarity anchors the
        // fill; direction selects which way the begin->end range travels.
        float value = behavior_clamp(
          effective_float(L, "value", L.arc_value, bag), 0.0f, 1.0f);
        float begin_deg = effective_float(L, "begin_angle", L.arc_begin_deg, bag);
        float end_deg   = effective_float(L, "end_angle",   L.arc_end_deg,   bag);
        int   polarity  = effective_int  (L, "polarity",    L.arc_polarity,  bag);
        int   direction = effective_int  (L, "direction",   L.arc_direction, bag);

        const float D2R = BEHAVIOR_PI / 180.0f;
        // 0 deg = 12 o'clock, cw-positive -> renderer radians (0 = 3 o'clock,
        // +y down): subtract 90 deg.
        float a0 = (begin_deg - 90.0f) * D2R;
        float a1 = (end_deg   - 90.0f) * D2R;
        // Resolve the begin->end sweep into the chosen direction so the range
        // is unambiguous however the angles were authored: cw increases the
        // angle, ccw decreases it.
        if (direction == 0) { while (a1 < a0) a1 += 2.0f * BEHAVIOR_PI; }
        else                { while (a1 > a0) a1 -= 2.0f * BEHAVIOR_PI; }
        const float total = a1 - a0;
        const float theta = a0 + total * value;
        float anchor;
        switch (polarity) {
          case 1:  anchor = (a0 + a1) * 0.5f; break;  // center (bipolar)
          case 2:  anchor = a1;               break;  // max (end)
          default: anchor = a0;               break;  // min (begin)
        }
        if (std::fabs(theta - anchor) < 1e-4f) break;  // nothing swept

        const float cx = r.x + r.w * 0.5f;
        const float cy = r.y + r.h * 0.5f;
        // Pie fills the inscribed ellipse; the ring insets by half its width
        // so the stroke stays within the layer rect.
        const float rx_fill = r.w * 0.5f;
        const float ry_fill = r.h * 0.5f;
        float rx_ring = rx_fill - sw * 0.5f;
        float ry_ring = ry_fill - sw * 0.5f;
        if (rx_ring < 0.0f) rx_ring = 0.0f;
        if (ry_ring < 0.0f) ry_ring = 0.0f;

        // Both the pie fill and the ring stroke start at `anchor`; evaluate the
        // trig once and reuse it for either path.
        const float cos_a = std::cos(anchor);
        const float sin_a = std::sin(anchor);

        if (has_fill && rx_fill > 0.0f && ry_fill > 0.0f) {
          k_painter_api.begin_path(p);
          k_painter_api.move_to(p, cx, cy);
          k_painter_api.line_to(p, cx + rx_fill * cos_a,
                                    cy + ry_fill * sin_a);
          append_elliptical_arc(p, cx, cy, rx_fill, ry_fill, anchor, theta);
          k_painter_api.close_path(p);
          k_painter_api.fill_path(p, fill);
        }
        if (has_stroke && rx_ring > 0.0f && ry_ring > 0.0f) {
          k_painter_api.begin_path(p);
          k_painter_api.move_to(p, cx + rx_ring * cos_a,
                                    cy + ry_ring * sin_a);
          append_elliptical_arc(p, cx, cy, rx_ring, ry_ring, anchor, theta);
          if (L.stroke_cap != 0 && k_painter_api.stroke_path_styled) {
            neui_stroke_style_t style{};
            style.cap         = static_cast<neui_line_cap_t>(L.stroke_cap);
            style.join        = NEUI_LINE_JOIN_ROUND;
            style.miter_limit = 4.0f;
            style.dash_array  = nullptr;
            style.dash_count  = 0;
            style.dash_offset = 0.0f;
            k_painter_api.stroke_path_styled(p, sw, stroke, &style);
          } else {
            k_painter_api.stroke_path(p, sw, stroke);
          }
        }
        break;
      }
      case NEUI_COMPOUND_LAYER_QR: {
        std::string text = qr_resolve_text(L, bag);

        // Dark colour: explicit "fill_color" (qr_dark != 0) else theme
        // text_primary so it tracks light / dark mode. Background: qr_background
        // (0 = transparent). ECC + quiet zone from their props.
        uint32_t dark = (L.qr_dark != 0u)
          ? L.qr_dark : neui_detail::color(neui_detail::ColorRole::text_primary);
        uint32_t bg    = L.qr_background;
        int      ecc   = L.qr_ecc;
        int      quiet = L.qr_quiet;

        // Target square side in physical pixels (the symbol is square; we
        // letterbox within a non-square rect).
        float scale = k_painter_api.get_scale_factor(p);
        if (scale <= 0.0f) scale = 1.0f;
        float side_logical = (r.w < r.h) ? r.w : r.h;
        uint32_t side_px =
          static_cast<uint32_t>(side_logical * scale + 0.5f);

        // The per-widget string is the cache key, so two widgets sharing this
        // layer with different NEUI_ATTR_QRCODE values each get their own
        // symbol rather than fighting over one held bitmap.
        CompoundLayer::QrSymbol* sym =
          qr_get_symbol(L, text, side_px, dark, bg, ecc, quiet, scale);
        if (!sym) break;  // empty / un-encodable text - draw nothing

        void* bmp = qr_upload(p, *sym);
        if (bmp && p->backend && p->backend->draw_bitmap && sym->w_px > 0) {
          // Draw at native resolution (bitmap logical size = w_px / scale),
          // centred within the layer rect to keep it square + crisp.
          float dw = static_cast<float>(sym->w_px) / sym->scale;
          float dh = static_cast<float>(sym->h_px) / sym->scale;
          float dx = r.x + (r.w - dw) * 0.5f;
          float dy = r.y + (r.h - dh) * 0.5f;
          // Pixel-snap the origin to the device grid. The bitmap is 1:1 with
          // physical pixels (w_px == dw*scale), so a fractional physical
          // origin - which a logical centre offset hits at non-integer DPI
          // like 150% - would make the backend bilinear-resample it, smearing
          // module edges and making some lines look a pixel wider than others.
          // Snapping so dx*scale / dy*scale are whole pixels keeps every module
          // a uniform width. (Each CUSTOMDRAW ctx origin is itself pixel-
          // aligned, so snapping in local space lands on the physical grid.)
          if (scale > 0.0f) {
            dx = static_cast<float>(std::lround(dx * scale)) / scale;
            dy = static_cast<float>(std::lround(dy * scale)) / scale;
          }
          p->backend->draw_bitmap(p->ctx, bmp,
                                  0.0f, 0.0f, 0.0f, 0.0f,  // full bitmap
                                  dx, dy, dw, dh, 0xFFFFFFFFu);
        }
        break;
      }
      case NEUI_COMPOUND_LAYER_GROUP: {
        // Container: establish a local coordinate frame at the group rect and
        // clip children to it, then paint every direct child sorted by its own
        // z. The group is one layer at the widget level (its z already decided
        // below/above in paint_compound_pass), so children paint together here
        // regardless of their z sign - no interleaving with the widget's real
        // children. Children read the same widget AttrBag, anchored / sized
        // against the group rect (r.w x r.h). The alpha scope opened above
        // wraps the whole subtree, so a group alpha fades all children at once.
        if (depth >= k_compound_max_group_depth) break;
        k_painter_api.push_transform(p);
        k_painter_api.translate(p, r.x, r.y);
        k_painter_api.push_clip(p, 0.0f, 0.0f, r.w, r.h);
        for (uint32_t cslot : compound_sorted_children(ca, self_slot)) {
          const CompoundLayer* child = compound_get_layer(ca, cslot);
          if (child)
            paint_compound_layer(p, ca, cslot, *child, r.w, r.h,
                                  bag, state_mask, depth + 1);
        }
        k_painter_api.pop_clip(p);
        k_painter_api.pop_transform(p);
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
    // Only top-level layers (parent == 0) interleave with the widget's real
    // children via the z<0 / z>=0 split; a GROUP's own children are painted
    // recursively from the GROUP case, scoped to the group's single z slot.
    auto slots = compound_sorted_children(ca, /*parent_slot*/0);
    for (uint32_t slot : slots) {
      const CompoundLayer* L = compound_get_layer(ca, slot);
      if (!L) continue;
      bool is_above = (L->z >= 0);
      if (is_above != paint_above_children) continue;
      paint_compound_layer(p, ca, slot, *L, widget_w, widget_h,
                            bag, state_mask, /*depth*/0);
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
