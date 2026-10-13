// HTML5 Canvas 2D rendering backend for the crossplatform host on the web.
//
// Shape mirror of backends/cairo/cairo_backend.cpp (the closest relative:
// Y-down, an immediate-mode 2D API with a save/restore state stack), adapted
// to Canvas 2D. Key differences from Cairo:
//   - Every call crosses into JavaScript (EM_JS). The JS half keeps one state
//     object, Module.neuiC2D: the contexts, the bitmaps (each a canvas), the
//     registered fonts and small caches. The C++ half keeps what the host
//     reads back synchronously: the alpha and font stacks, the damage state
//     and a text-measure cache.
//   - The canvas keeps its pixels between frames, so a partial frame only
//     clips and clears its damage; nothing needs blitting at end_frame (the
//     browser presents the canvas when the task yields).
//   - The canvas' current path is not part of save / restore and is not
//     touched by fillRect / strokeRect / fillText, so the path survives other
//     draws without Cairo's PathAside. Clips use a separate Path2D for the
//     same reason. Path points are mapped by the transform current when they
//     are added, as in Cairo and CG.
//   - Text is laid out by the browser (shaping, fallback fonts). draw_text
//     reproduces Cairo's box rules: split on '\n', centre the line block
//     vertically using the font's ascent + descent, clip to the box.
//   - Canvas 2D gradients have no REPEAT / MIRROR; they degrade to CLAMP, as
//     on CG.
//
// All coordinates are logical px at 96 DPI; colours are 0xAARRGGBB; bitmaps
// arrive as BGRA8 premultiplied and are converted to the straight RGBA that
// ImageData holds (lossy for very low alpha, as every putImageData is).

#include <emscripten.h>

#include <cstdio>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "canvas2d_backend.h"
#include "../shared/backend_util.h"

// ---------------------------------------------------------------------------
// JavaScript half. c2d_setup installs Module.neuiC2D once; every other
// function reads it. Context and bitmap ids index S.ctx / S.bmp (0 = none).

EM_JS_DEPS(neui_canvas2d, "$UTF8ToString");

EM_JS(void, c2d_setup, (), {
  if (Module.neuiC2D) return;
  var S = Module.neuiC2D = {
    ctx: [null], bmp: [null], fonts: {}, nextFont: 1,
    cols: new Map(), metrics: new Map(), mfont: null, mctx: null,
  };
  // An off-screen canvas: OffscreenCanvas where it exists, else a detached
  // <canvas> (Playwright's WebKit build has no OffscreenCanvas).
  S.mk = function(w, h) {
    if (typeof OffscreenCanvas !== 'undefined') return new OffscreenCanvas(w, h);
    var c = document.createElement('canvas'); c.width = w; c.height = h; return c;
  };
  // 0xAARRGGBB times an alpha multiplier -> CSS colour; opaque-multiplier
  // strings are cached per colour.
  S.col = function(argb, mul) {
    argb = argb >>> 0;
    if (mul === 1) { var s = S.cols.get(argb); if (s !== undefined) return s; }
    var a = ((argb >>> 24) / 255) * mul;
    var str = 'rgba(' + ((argb >>> 16) & 255) + ',' + ((argb >>> 8) & 255) + ',' +
              (argb & 255) + ',' + a + ')';
    if (mul === 1) S.cols.set(argb, str);
    return str;
  };
  // Ascent + descent of a CSS font (the line box Cairo's font extents give).
  S.metric = function(font) {
    var m = S.metrics.get(font);
    if (m) return m;
    S.mctx.font = font; S.mfont = font;
    var t = S.mctx.measureText('Mg');
    // (No backslashes: an EM_JS body is a C string literal.)
    var px = /([0-9.]+)px/.exec(font);
    var size = (px && parseFloat(px[1])) || 12;
    var asc = t.fontBoundingBoxAscent, desc = t.fontBoundingBoxDescent;
    if (!(asc > 0)) asc = size * 0.8;
    if (!(desc >= 0)) desc = size * 0.2;
    m = [asc, desc];
    S.metrics.set(font, m);
    return m;
  };
  S.mctx = S.mk(1, 1).getContext('2d');
  S.add = function(arr, v) { for (var i = 1; i < arr.length; ++i) if (!arr[i]) { arr[i] = v; return i; } arr.push(v); return arr.length - 1; };
  S.rect = function(x, y, w, h) { var p = new Path2D(); p.rect(x, y, w, h); return p; };
  S.grad = function(c, kind, sx, sy, ex, ey, r, sp, n, mul) {
    // Radial: the start point is the centre, the end point the focal point
    // (gradient.h), so the focal circle (radius 0) is the inner one.
    var g = (kind === 1) ? c.createRadialGradient(ex, ey, 0, sx, sy, Math.max(r, 0))
                         : c.createLinearGradient(sx, sy, ex, ey);
    for (var i = 0; i < n; ++i) {
      var off = HEAPF32[(sp >> 2) + i * 2];
      var argb = HEAPU32[(sp >> 2) + i * 2 + 1];
      g.addColorStop(Math.min(1, Math.max(0, off)), S.col(argb, mul));
    }
    return g;
  };
  S.style = function(c, lw, cap, join, miter, dp, dn, doff) {
    c.lineWidth = lw;
    c.lineCap = cap === 1 ? 'round' : cap === 2 ? 'square' : 'butt';
    c.lineJoin = join === 1 ? 'round' : join === 2 ? 'bevel' : 'miter';
    c.miterLimit = miter > 0 ? miter : 4;
    var d = [];
    for (var i = 0; i < dn; ++i) d.push(HEAPF32[(dp >> 2) + i]);
    c.setLineDash(d); c.lineDashOffset = doff;
  };
});

EM_JS(int, c2d_ctx_create, (int canvas_id, int w, int h, int offscreen), {
  var S = Module.neuiC2D, canvas;
  if (offscreen) {
    canvas = S.mk(w, h);
  } else {
    canvas = Module.neuiCanvases && Module.neuiCanvases[canvas_id];
    if (!canvas) return 0;
    canvas.width = w; canvas.height = h;
  }
  var c = canvas.getContext('2d', offscreen ? { willReadFrequently: true } : { alpha: true });
  if (!c) return 0;
  var st = { canvas: canvas, c: c, depth: 0, gen: 0 };
  if (!offscreen && canvas.addEventListener) {
    canvas.addEventListener('contextlost', function() { st.gen++; });
    canvas.addEventListener('contextrestored', function() { st.gen++; });
  }
  return S.add(S.ctx, st);
});

EM_JS(void, c2d_ctx_destroy, (int id), { Module.neuiC2D.ctx[id] = null; });

EM_JS(void, c2d_ctx_resize, (int id, int w, int h), {
  var st = Module.neuiC2D.ctx[id]; if (!st) return;
  st.canvas.width = w; st.canvas.height = h;   // clears the backing store
  st.depth = 0;
});

EM_JS(int, c2d_ctx_gen, (int id), {
  var st = Module.neuiC2D.ctx[id]; return st ? st.gen : 0;
});

EM_JS(void, c2d_begin, (int id, float scale, uint32_t clear, const int* rects, int n), {
  var S = Module.neuiC2D, st = S.ctx[id]; if (!st) return;
  var c = st.c, W = st.canvas.width, H = st.canvas.height;
  while (st.depth > 0) { c.restore(); st.depth--; }
  c.setTransform(1, 0, 0, 1, 0, 0);
  c.globalAlpha = 1;
  c.globalCompositeOperation = 'source-over';
  c.save(); st.depth = 1;
  if (n > 0) {
    var p = new Path2D(), b = rects >> 2;
    for (var i = 0; i < n; ++i)
      p.rect(HEAP32[b + i * 4], HEAP32[b + i * 4 + 1], HEAP32[b + i * 4 + 2], HEAP32[b + i * 4 + 3]);
    c.clip(p);
  }
  c.clearRect(0, 0, W, H);
  if ((clear >>> 24) !== 0) { c.fillStyle = S.col(clear, 1); c.fillRect(0, 0, W, H); }
  c.setTransform(scale, 0, 0, scale, 0, 0);
  c.beginPath();
});

EM_JS(void, c2d_end, (int id), {
  var st = Module.neuiC2D.ctx[id]; if (!st) return;
  while (st.depth > 0) { st.c.restore(); st.depth--; }
});

EM_JS(void, c2d_fill_rect, (int id, float x, float y, float w, float h, uint32_t argb, float a), {
  var S = Module.neuiC2D, st = S.ctx[id]; if (!st) return;
  st.c.fillStyle = S.col(argb, a); st.c.fillRect(x, y, w, h);
});

EM_JS(void, c2d_stroke_rect, (int id, float x, float y, float w, float h, float lw, uint32_t argb, float a), {
  var S = Module.neuiC2D, st = S.ctx[id]; if (!st) return;
  var c = st.c;
  c.strokeStyle = S.col(argb, a); c.lineWidth = lw; c.setLineDash([]);
  c.strokeRect(x, y, w, h);
});

EM_JS(void, c2d_text, (int id, float x, float y, float w, float h, const char* tp, int tl,
                       const char* fp, uint32_t argb, float a), {
  var S = Module.neuiC2D, st = S.ctx[id]; if (!st) return;
  var c = st.c, font = UTF8ToString(fp), text = UTF8ToString(tp, tl);
  var m = S.metric(font), lh = m[0] + m[1];
  var lines = text.split('\n');
  var top = y + (h - lh * lines.length) * 0.5;
  if (top < y) top = y;
  var base = top + m[0];
  c.save();
  c.clip(S.rect(x, y, w, h));
  c.font = font; c.textBaseline = 'alphabetic'; c.textAlign = 'left';
  c.fillStyle = S.col(argb, a);
  for (var i = 0; i < lines.length; ++i) {
    var ln = lines[i];
    if (ln.length && ln.charCodeAt(ln.length - 1) === 13) ln = ln.slice(0, -1);
    if (ln.length) c.fillText(ln, x, base);
    base += lh;
  }
  c.restore();
});

EM_JS(double, c2d_measure, (const char* tp, int tl, const char* fp), {
  var S = Module.neuiC2D, font = UTF8ToString(fp);
  if (S.mfont !== font) { S.mctx.font = font; S.mfont = font; }
  return S.mctx.measureText(UTF8ToString(tp, tl)).width;
});

EM_JS(void, c2d_clip, (int id, float x, float y, float w, float h), {
  var S = Module.neuiC2D, st = S.ctx[id]; if (!st) return;
  st.c.save(); st.depth++;
  st.c.clip(S.rect(x, y, w, h));
});

EM_JS(void, c2d_save, (int id), {
  var st = Module.neuiC2D.ctx[id]; if (!st) return;
  st.c.save(); st.depth++;
});

// pop_clip / pop_transform. The frame's own outer save (depth 1) is never
// popped by an unbalanced client pop.
EM_JS(void, c2d_restore, (int id), {
  var st = Module.neuiC2D.ctx[id]; if (!st || st.depth <= 1) return;
  st.c.restore(); st.depth--;
});

EM_JS(void, c2d_xform, (int id, int op, float a, float b), {
  var st = Module.neuiC2D.ctx[id]; if (!st) return;
  if (op === 0) st.c.translate(a, b);
  else if (op === 1) st.c.rotate(a);
  else st.c.scale(a, b);
});

EM_JS(void, c2d_get_transform, (int id, float* out), {
  var st = Module.neuiC2D.ctx[id]; if (!st) return;
  var m = st.c.getTransform(), o = out >> 2;
  HEAPF32[o] = m.a; HEAPF32[o + 1] = m.b; HEAPF32[o + 2] = m.c;
  HEAPF32[o + 3] = m.d; HEAPF32[o + 4] = m.e; HEAPF32[o + 5] = m.f;
});

// Path verbs: 0 begin, 1 move, 2 line, 3 close, 4 quad, 5 cubic, 6 arc.
EM_JS(void, c2d_path, (int id, int op, float a, float b, float c_, float d, float e, float f), {
  var st = Module.neuiC2D.ctx[id]; if (!st) return;
  var c = st.c;
  switch (op) {
    case 0: c.beginPath(); break;
    case 1: c.moveTo(a, b); break;
    case 2: c.lineTo(a, b); break;
    case 3: c.closePath(); break;
    case 4: c.quadraticCurveTo(a, b, c_, d); break;
    case 5: c.bezierCurveTo(a, b, c_, d, e, f); break;
    // Y-down: a positive sweep is clockwise on screen (renderer.h), which is
    // the canvas' default direction; a reverse sweep goes anticlockwise.
    case 6: c.arc(a, b, Math.max(c_, 0), d, e, e < d); break;
  }
});

EM_JS(void, c2d_fill_path, (int id, uint32_t argb, float a, int evenodd), {
  var S = Module.neuiC2D, st = S.ctx[id]; if (!st) return;
  st.c.fillStyle = S.col(argb, a);
  st.c.fill(evenodd ? 'evenodd' : 'nonzero');
});

EM_JS(void, c2d_stroke_path, (int id, float lw, uint32_t argb, float a,
                              int cap, int join, float miter, const float* dp, int dn, float doff), {
  var S = Module.neuiC2D, st = S.ctx[id]; if (!st) return;
  var c = st.c;
  c.save();
  S.style(c, lw, cap, join, miter, dp, dn, doff);
  c.strokeStyle = S.col(argb, a);
  c.stroke();
  c.restore();
});

// Gradient fills / strokes. what: 0 = rect, 1 = path fill, 2 = path stroke.
EM_JS(void, c2d_gradient, (int id, int what, float x, float y, float w, float h,
                           int kind, float sx, float sy, float ex, float ey, float r,
                           const void* stops, int n, float a, int evenodd,
                           float lw, int cap, int join, float miter,
                           const float* dp, int dn, float doff), {
  var S = Module.neuiC2D, st = S.ctx[id]; if (!st) return;
  var c = st.c, g = S.grad(c, kind, sx, sy, ex, ey, r, stops, n, a);
  if (what === 0) { c.fillStyle = g; c.fillRect(x, y, w, h); }
  else if (what === 1) { c.fillStyle = g; c.fill(evenodd ? 'evenodd' : 'nonzero'); }
  else {
    c.save();
    S.style(c, lw, cap, join, miter, dp, dn, doff);
    c.strokeStyle = g;
    c.stroke();
    c.restore();
  }
});

EM_JS(int, c2d_bmp_create, (int w, int h, const uint8_t* rgba), {
  var S = Module.neuiC2D;
  var cv = S.mk(w, h), g = cv.getContext('2d');
  if (!g) return 0;
  var img = g.createImageData(w, h);
  img.data.set(HEAPU8.subarray(rgba, rgba + w * h * 4));
  g.putImageData(img, 0, 0);
  return S.add(S.bmp, { c: cv, w: w, h: h, tints: null });
});

EM_JS(void, c2d_bmp_destroy, (int id), { Module.neuiC2D.bmp[id] = null; });

// Draw a bitmap region (source in bitmap pixels). tinted != 0 multiplies the
// pixels by the tint's RGB inside the bitmap's own alpha (a cached copy per
// tint), the multiplicative tint the d2d / cg / cairo backends apply.
EM_JS(void, c2d_bmp_draw, (int id, int bid, float sx, float sy, float sw, float sh,
                           float dx, float dy, float dw, float dh, float a,
                           int tinted, uint32_t tint), {
  var S = Module.neuiC2D, st = S.ctx[id], b = S.bmp[bid]; if (!st || !b) return;
  var src = b.c;
  if (tinted) {
    if (!b.tints) b.tints = new Map();
    src = b.tints.get(tint >>> 0);
    if (!src) {
      src = S.mk(b.w, b.h);
      var g = src.getContext('2d');
      g.drawImage(b.c, 0, 0);
      g.globalCompositeOperation = 'multiply';
      g.fillStyle = S.col((tint | 0xFF000000) >>> 0, 1);
      g.fillRect(0, 0, b.w, b.h);
      g.globalCompositeOperation = 'destination-in';
      g.drawImage(b.c, 0, 0);
      b.tints.set(tint >>> 0, src);
    }
  }
  var c = st.c;
  c.globalAlpha = a;
  c.drawImage(src, sx, sy, sw, sh, dx, dy, dw, dh);
  c.globalAlpha = 1;
});

// Off-screen readback: straight RGBA -> premultiplied BGRA, tightly packed.
EM_JS(int, c2d_read_pixels, (int id, uint8_t* out, int w, int h), {
  var st = Module.neuiC2D.ctx[id]; if (!st) return 0;
  var d = st.c.getImageData(0, 0, w, h).data, o = HEAPU8, n = w * h * 4;
  for (var i = 0; i < n; i += 4) {
    var al = d[i + 3];
    o[out + i]     = (d[i + 2] * al + 127) / 255 | 0;
    o[out + i + 1] = (d[i + 1] * al + 127) / 255 | 0;
    o[out + i + 2] = (d[i]     * al + 127) / 255 | 0;
    o[out + i + 3] = al;
  }
  return 1;
});

// A client font: the bytes are copied, so the caller's buffer may go. An
// ArrayBuffer FontFace is usable as soon as it is constructed (Stage 0).
EM_JS(int, c2d_font_add, (const char* fam, const uint8_t* data, int len, int weight), {
  var S = Module.neuiC2D;
  try {
    var bytes = HEAPU8.slice(data, data + len);
    var f = new FontFace(UTF8ToString(fam), bytes.buffer, { weight: String(weight) });
    var set = (typeof document !== 'undefined') ? document.fonts : self.fonts;
    set.add(f);
    var tok = S.nextFont++;
    S.fonts[tok] = f;
    S.metrics.clear(); S.mfont = null;
    return tok;
  } catch (e) {
    return 0;
  }
});

// The app leaves the page: its fonts leave the document, the only page-wide
// reference the backend holds. Everything else lives on the module object
// and goes with the instance; the backend stays callable (the client's own
// cleanup after run() still destroys contexts and bitmaps).
EM_JS(void, c2d_shutdown, (), {
  var S = Module.neuiC2D; if (!S) return;
  var set = (typeof document !== 'undefined') ? document.fonts : self.fonts;
  for (var k in S.fonts) { try { set.delete(S.fonts[k]); } catch (e) {} }
  S.fonts = {};
  S.metrics.clear(); S.mfont = null;
});

EM_JS(void, c2d_font_remove, (int tok), {
  var S = Module.neuiC2D, f = S.fonts[tok]; if (!f) return;
  var set = (typeof document !== 'undefined') ? document.fonts : self.fonts;
  set.delete(f);
  delete S.fonts[tok];
  S.metrics.clear(); S.mfont = null;
});

namespace neui_canvas2d_backend
{
  struct FontState
  {
    std::string family;     // empty = system default
    int         weight = 0; // CSS 100..900; 0 = Regular (400)
  };

  struct C2DCtx
  {
    int      id = 0;                // index into Module.neuiC2D.ctx
    uint32_t w_px = 0, h_px = 0;    // backing store, physical px
    float    scale = 1.0f;          // physical px per logical px
    bool     is_offscreen = false;
    bool     eo_fill = false;       // even-odd fill rule (reset on begin_path)

    std::vector<float>     alpha_stack;
    std::vector<FontState> font_stack;

    // CSS font string cache for the active (family, weight, size).
    std::string css;
    std::string css_family;
    int         css_weight = -1;
    uint32_t    css_size_q10 = 0;

    // Partial repaint, as in the Cairo backend: the canvas keeps its pixels
    // except after a resize (the backing store is reallocated), a scale
    // change or a lost context, so until one full frame has been drawn under
    // the current conditions every frame is full.
    std::vector<float> pending_damage;
    std::vector<int>   frame_damage_px;
    bool     has_content = false;
    float    content_scale = 0.0f;
    int      content_gen = 0;
  };

  // --------------------------------------------------------------------------
  // Helpers

  static void ensure_setup()
  {
    static bool done = false;
    if (!done) { c2d_setup(); done = true; }
  }

  static inline float current_alpha(const C2DCtx* st)
  {
    return neui_detail::alpha_stack_current(st->alpha_stack);
  }

  // A CSS font family list for `family`: quoted (with " and \ escaped), then
  // the system UI fallbacks. Empty = the system UI font.
  static void append_family(std::string& out, const std::string& family)
  {
    if (!family.empty()) {
      out += '"';
      for (char ch : family) {
        if (ch == '"' || ch == '\\') out += '\\';
        out += ch;
      }
      out += "\", ";
    }
    out += "system-ui, -apple-system, \"Segoe UI\", Roboto, sans-serif";
  }

  // The CSS font for the context's active font at `font_size` (logical px;
  // the context transform carries the device scale). Rebuilt only when the
  // family, weight or 0.1-px size bucket changes.
  static const std::string& css_font(C2DCtx* st, float font_size)
  {
    static const FontState k_default;
    const FontState& fs = st->font_stack.empty() ? k_default : st->font_stack.back();
    const uint32_t q = neui_detail::font_size_q10(font_size);
    if (st->css_weight == fs.weight && st->css_size_q10 == q && st->css_family == fs.family &&
        !st->css.empty())
      return st->css;
    st->css_family = fs.family;
    st->css_weight = fs.weight;
    st->css_size_q10 = q;
    char head[48];
    std::snprintf(head, sizeof head, "%d %.1fpx ", fs.weight > 0 ? fs.weight : 400,
                  static_cast<double>(q) / 10.0);
    st->css = head;
    append_family(st->css, fs.family);
    return st->css;
  }

  // Text widths are asked for constantly (caret math, wrapping, auto-size)
  // and never change for a given font + string, so they are cached. Cleared
  // wholesale when it grows large or a font is registered / unregistered.
  static std::unordered_map<std::string, float>& measure_cache()
  {
    static std::unordered_map<std::string, float> m;
    return m;
  }

  // --------------------------------------------------------------------------
  // Lifecycle

  static neui_render_ctx_t c2d_create_context(void* native_handle,
                                              uint32_t width, uint32_t height)
  {
    ensure_setup();
    if (width == 0)  width  = 1;
    if (height == 0) height = 1;
    const int canvas_id = static_cast<int>(reinterpret_cast<uintptr_t>(native_handle));
    const int id = c2d_ctx_create(canvas_id, static_cast<int>(width),
                                  static_cast<int>(height), 0);
    if (!id) return nullptr;
    auto* st = new C2DCtx();
    st->id = id;
    st->w_px = width;
    st->h_px = height;
    return st;
  }

  static void c2d_destroy_context(neui_render_ctx_t raw)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (!st) return;
    c2d_ctx_destroy(st->id);
    delete st;
  }

  static void c2d_resize(neui_render_ctx_t raw, uint32_t width, uint32_t height)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (!st || st->is_offscreen) return;   // offscreen ctxs are fixed-size
    if (width == 0)  width  = 1;
    if (height == 0) height = 1;
    if (width == st->w_px && height == st->h_px) return;
    st->w_px = width;
    st->h_px = height;
    c2d_ctx_resize(st->id, static_cast<int>(width), static_cast<int>(height));
    st->has_content = false;
  }

  static void c2d_begin_frame(neui_render_ctx_t raw, uint32_t clear_argb)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (!st) return;
    st->alpha_stack.clear();
    st->font_stack.clear();

    // Partial frame: clip to the queued damage in device px, snapped outward
    // (pending_damage is only non-empty when set_frame_damage accepted it).
    st->frame_damage_px.clear();
    const auto& pd = st->pending_damage;
    for (size_t i = 0; i + 3 < pd.size(); i += 4) {
      int r[4];
      if (neui_detail::damage_rect_to_device(pd[i], pd[i + 1], pd[i + 2], pd[i + 3], st->scale,
                                             static_cast<int>(st->w_px),
                                             static_cast<int>(st->h_px), r))
        st->frame_damage_px.insert(st->frame_damage_px.end(), r, r + 4);
    }
    int n = static_cast<int>(st->frame_damage_px.size() / 4);
    // Accepted damage that lies entirely off the surface leaves nothing to
    // repaint: an empty clip (one zero-size rect) draws nothing - correct,
    // since nothing visible changed.
    if (!pd.empty() && n == 0) {
      st->frame_damage_px.assign({0, 0, 0, 0});
      n = 1;
    }
    st->pending_damage.clear();
    c2d_begin(st->id, st->scale, clear_argb, st->frame_damage_px.data(), n);
  }

  static void c2d_end_frame(neui_render_ctx_t raw)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (!st) return;
    c2d_end(st->id);
    st->has_content   = true;
    st->content_scale = st->scale;
    st->content_gen   = c2d_ctx_gen(st->id);
    st->frame_damage_px.clear();
  }

  static bool c2d_set_frame_damage(neui_render_ctx_t raw, const float* rects_xywh, int count)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (!st) return false;
    st->pending_damage.clear();
    if (!rects_xywh || count <= 0) return false;
    if (!st->has_content || st->content_scale != st->scale ||
        st->content_gen != c2d_ctx_gen(st->id))
      return false;
    st->pending_damage.assign(rects_xywh, rects_xywh + static_cast<size_t>(count) * 4);
    return true;
  }

  // --------------------------------------------------------------------------
  // Primitives

  static void c2d_fill_rect_fn(neui_render_ctx_t raw, float x, float y, float w, float h,
                               uint32_t argb)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_fill_rect(st->id, x, y, w, h, argb, current_alpha(st));
  }

  // Strokes the exact rect, centred on the edge (d2d DrawRectangle, cg
  // CGContextStrokeRect, cairo_rectangle + stroke all do the same).
  static void c2d_draw_rect_fn(neui_render_ctx_t raw, float x, float y, float w, float h,
                               float stroke_width, uint32_t argb)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_stroke_rect(st->id, x, y, w, h, stroke_width, argb, current_alpha(st));
  }

  static float c2d_get_scale_factor(neui_render_ctx_t raw)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    return st ? st->scale : 1.0f;
  }

  static void c2d_update_dpi(neui_render_ctx_t raw, uint32_t dpi)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (!st || dpi == 0) return;
    st->scale = static_cast<float>(dpi) / 96.0f;
  }

  static void c2d_draw_text(neui_render_ctx_t raw, float x, float y, float w, float h,
                            const char* text, float font_size, uint32_t argb)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (!st || !text || !*text || font_size <= 0.0f) return;
    const std::string& font = css_font(st, font_size);
    c2d_text(st->id, x, y, w, h, text, static_cast<int>(std::strlen(text)), font.c_str(),
             argb, current_alpha(st));
  }

  static float c2d_measure_text(neui_render_ctx_t raw, const char* text, int text_len,
                                float font_size)
  {
    ensure_setup();
    if (!text || font_size <= 0.0f) return 0.0f;
    const size_t len = text_len < 0 ? std::strlen(text) : static_cast<size_t>(text_len);
    if (len == 0) return 0.0f;
    // A context only contributes its font stack; without one (or before the
    // first frame) the default font applies.
    C2DCtx scratch;
    C2DCtx* st = raw ? static_cast<C2DCtx*>(raw) : &scratch;
    const std::string& font = css_font(st, font_size);

    std::string key;
    key.reserve(font.size() + 1 + len);
    key.append(font).append(1, '\x1f').append(text, len);
    auto& cache = measure_cache();
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;
    if (cache.size() > 20000) cache.clear();
    const float w = static_cast<float>(c2d_measure(text, static_cast<int>(len), font.c_str()));
    cache.emplace(std::move(key), w);
    return w;
  }

  static void c2d_push_clip(neui_render_ctx_t raw, float x, float y, float w, float h)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_clip(st->id, x, y, w, h);
  }

  static void c2d_pop_clip(neui_render_ctx_t raw)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_restore(st->id);
  }

  // --------------------------------------------------------------------------
  // Bitmaps - a canvas each, shared by every context (any canvas can draw
  // any other), so the ctx argument of create / destroy is unused.

  struct C2DBitmap
  {
    int      id = 0;
    uint32_t width_px = 0, height_px = 0;
    float    scale = 1.0f;
  };

  static void* c2d_create_bitmap(neui_render_ctx_t /*raw*/, uint32_t width_px, uint32_t height_px,
                                 const uint8_t* bgra, float scale)
  {
    ensure_setup();
    if (!bgra || width_px == 0 || height_px == 0) return nullptr;
    if (scale <= 0.0f) scale = 1.0f;
    // Premultiplied BGRA -> straight RGBA (what ImageData holds).
    const size_t n = static_cast<size_t>(width_px) * height_px * 4u;
    std::vector<uint8_t> rgba(n);
    for (size_t i = 0; i < n; i += 4) {
      const uint32_t a = bgra[i + 3];
      if (a == 0) { rgba[i] = rgba[i + 1] = rgba[i + 2] = rgba[i + 3] = 0; continue; }
      if (a == 255) {
        rgba[i] = bgra[i + 2]; rgba[i + 1] = bgra[i + 1]; rgba[i + 2] = bgra[i]; rgba[i + 3] = 255;
        continue;
      }
      auto un = [a](uint32_t c) {
        uint32_t v = (c * 255u + a / 2u) / a;
        return static_cast<uint8_t>(v > 255u ? 255u : v);
      };
      rgba[i] = un(bgra[i + 2]); rgba[i + 1] = un(bgra[i + 1]); rgba[i + 2] = un(bgra[i]);
      rgba[i + 3] = static_cast<uint8_t>(a);
    }
    const int id = c2d_bmp_create(static_cast<int>(width_px), static_cast<int>(height_px),
                                  rgba.data());
    if (!id) return nullptr;
    auto* b = new C2DBitmap();
    b->id = id; b->width_px = width_px; b->height_px = height_px; b->scale = scale;
    return b;
  }

  static void c2d_destroy_bitmap(neui_render_ctx_t /*raw*/, void* bitmap)
  {
    auto* b = static_cast<C2DBitmap*>(bitmap);
    if (!b) return;
    c2d_bmp_destroy(b->id);
    delete b;
  }

  static void c2d_draw_bitmap(neui_render_ctx_t raw, void* bitmap,
                              float src_x, float src_y, float src_w, float src_h,
                              float dst_x, float dst_y, float dst_w, float dst_h, uint32_t tint)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    auto* b = static_cast<C2DBitmap*>(bitmap);
    if (!st || !b || dst_w <= 0.0f || dst_h <= 0.0f) return;
    // Source region in bitmap (physical) pixels; src_w/h <= 0 => the whole
    // bitmap (renderer.h). The source rect arrives in the bitmap's logical
    // units, hence the scale.
    const bool whole = !(src_w > 0.0f) || !(src_h > 0.0f);
    const float sx = whole ? 0.0f : src_x * b->scale;
    const float sy = whole ? 0.0f : src_y * b->scale;
    const float sw = whole ? static_cast<float>(b->width_px)  : src_w * b->scale;
    const float sh = whole ? static_cast<float>(b->height_px) : src_h * b->scale;
    if (sw <= 0.0f || sh <= 0.0f) return;
    float a = current_alpha(st);
    const bool tinted = tint != 0xFFFFFFFFu;
    if (tinted) a *= static_cast<float>((tint >> 24) & 0xFFu) / 255.0f;
    c2d_bmp_draw(st->id, b->id, sx, sy, sw, sh, dst_x, dst_y, dst_w, dst_h, a, tinted ? 1 : 0,
                 tint);
  }

  // --------------------------------------------------------------------------
  // Paths

  static void c2d_begin_path(neui_render_ctx_t raw)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (!st) return;
    st->eo_fill = false;
    c2d_path(st->id, 0, 0, 0, 0, 0, 0, 0);
  }

  static void c2d_move_to(neui_render_ctx_t raw, float x, float y)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_path(st->id, 1, x, y, 0, 0, 0, 0);
  }

  static void c2d_line_to(neui_render_ctx_t raw, float x, float y)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_path(st->id, 2, x, y, 0, 0, 0, 0);
  }

  static void c2d_arc(neui_render_ctx_t raw, float cx, float cy, float radius,
                      float start_rad, float end_rad)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_path(st->id, 6, cx, cy, radius, start_rad, end_rad, 0);
  }

  static void c2d_close_path(neui_render_ctx_t raw)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_path(st->id, 3, 0, 0, 0, 0, 0, 0);
  }

  static void c2d_quad_to(neui_render_ctx_t raw, float cx, float cy, float x, float y)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_path(st->id, 4, cx, cy, x, y, 0, 0);
  }

  static void c2d_cubic_to(neui_render_ctx_t raw, float c1x, float c1y, float c2x, float c2y,
                           float x, float y)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_path(st->id, 5, c1x, c1y, c2x, c2y, x, y);
  }

  static void c2d_set_fill_rule(neui_render_ctx_t raw, neui_fill_rule_t rule)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) st->eo_fill = (rule == NEUI_FILL_RULE_EVENODD);
  }

  static void c2d_fill_path_fn(neui_render_ctx_t raw, uint32_t argb)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_fill_path(st->id, argb, current_alpha(st), st->eo_fill ? 1 : 0);
  }

  static void c2d_stroke_path_styled(neui_render_ctx_t raw, float stroke_width, uint32_t argb,
                                     const neui_stroke_style_t* style)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (!st) return;
    if (!style) {
      c2d_stroke_path(st->id, stroke_width, argb, current_alpha(st), 0, 0, 4.0f, nullptr, 0, 0.0f);
      return;
    }
    const int dn = (style->dash_array && style->dash_count > 0)
                     ? static_cast<int>(style->dash_count) : 0;
    c2d_stroke_path(st->id, stroke_width, argb, current_alpha(st),
                    static_cast<int>(style->cap), static_cast<int>(style->join),
                    style->miter_limit, dn ? style->dash_array : nullptr, dn, style->dash_offset);
  }

  static void c2d_stroke_path_fn(neui_render_ctx_t raw, float stroke_width, uint32_t argb)
  {
    c2d_stroke_path_styled(raw, stroke_width, argb, nullptr);
  }

  // --------------------------------------------------------------------------
  // Gradients

  static void c2d_gradient_op(C2DCtx* st, int what, float x, float y, float w, float h,
                              const neui_gradient_t* g, float stroke_width,
                              const neui_stroke_style_t* style)
  {
    if (!st || !g || !g->stops || g->stop_count < 2) return;
    static_assert(sizeof(neui_gradient_stop_t) == 8, "the JS side reads stops as {f32, u32}");
    int cap = 0, join = 0, dn = 0;
    float miter = 4.0f, doff = 0.0f;
    const float* dp = nullptr;
    if (style) {
      cap = static_cast<int>(style->cap);
      join = static_cast<int>(style->join);
      miter = style->miter_limit;
      if (style->dash_array && style->dash_count > 0) {
        dp = style->dash_array;
        dn = static_cast<int>(style->dash_count);
        doff = style->dash_offset;
      }
    }
    c2d_gradient(st->id, what, x, y, w, h, g->kind == NEUI_GRADIENT_RADIAL ? 1 : 0,
                 g->start_x, g->start_y, g->end_x, g->end_y, g->radius,
                 g->stops, static_cast<int>(g->stop_count), current_alpha(st),
                 st->eo_fill ? 1 : 0, stroke_width, cap, join, miter, dp, dn, doff);
  }

  static void c2d_fill_rect_gradient(neui_render_ctx_t raw, float x, float y, float w, float h,
                                     const neui_gradient_t* g)
  {
    c2d_gradient_op(static_cast<C2DCtx*>(raw), 0, x, y, w, h, g, 0.0f, nullptr);
  }

  static void c2d_fill_path_gradient(neui_render_ctx_t raw, const neui_gradient_t* g)
  {
    c2d_gradient_op(static_cast<C2DCtx*>(raw), 1, 0, 0, 0, 0, g, 0.0f, nullptr);
  }

  static void c2d_stroke_path_gradient(neui_render_ctx_t raw, float stroke_width,
                                       const neui_gradient_t* g, const neui_stroke_style_t* style)
  {
    c2d_gradient_op(static_cast<C2DCtx*>(raw), 2, 0, 0, 0, 0, g, stroke_width, style);
  }

  // --------------------------------------------------------------------------
  // Transform stack - canvas save / restore, shared with the clip stack
  // exactly as Cairo's cairo_save / cairo_restore.

  static void c2d_push_transform(neui_render_ctx_t raw)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_save(st->id);
  }
  static void c2d_pop_transform(neui_render_ctx_t raw)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_restore(st->id);
  }
  static void c2d_translate(neui_render_ctx_t raw, float dx, float dy)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_xform(st->id, 0, dx, dy);
  }
  static void c2d_rotate(neui_render_ctx_t raw, float radians)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_xform(st->id, 1, radians, 0.0f);
  }
  static void c2d_scale(neui_render_ctx_t raw, float sx, float sy)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) c2d_xform(st->id, 2, sx, sy);
  }

  // The canvas transform maps user space straight to the backing store's
  // pixels (begin_frame sets the display scale), so it is the answer as is.
  static void c2d_get_device_transform(neui_render_ctx_t raw, float m[6])
  {
    auto* st = static_cast<C2DCtx*>(raw);
    const float s = st ? st->scale : 1.0f;
    m[0] = s; m[1] = 0.0f; m[2] = 0.0f; m[3] = s; m[4] = 0.0f; m[5] = 0.0f;
    if (st) c2d_get_transform(st->id, m);
  }

  static uint32_t c2d_get_context_generation(neui_render_ctx_t raw)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    return st ? static_cast<uint32_t>(c2d_ctx_gen(st->id)) : 0u;
  }

  // --------------------------------------------------------------------------
  // Alpha + font stacks (C++ only; folded into each draw)

  static void c2d_push_alpha(neui_render_ctx_t raw, float factor)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) neui_detail::alpha_stack_push(st->alpha_stack, factor);
  }
  static void c2d_pop_alpha(neui_render_ctx_t raw)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st) neui_detail::alpha_stack_pop(st->alpha_stack);
  }

  static void c2d_push_font(neui_render_ctx_t raw, const char* family_utf8, int weight)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (!st) return;
    FontState fs;
    if (family_utf8 && *family_utf8) fs.family = family_utf8;
    fs.weight = weight;
    st->font_stack.push_back(std::move(fs));
  }
  static void c2d_pop_font(neui_render_ctx_t raw)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (st && !st->font_stack.empty()) st->font_stack.pop_back();
  }

  // --------------------------------------------------------------------------
  // Client fonts. The browser needs the family name up front, so it is read
  // from the font's own `name` table (typographic family, else family), and
  // the weight from OS/2 so CSS weight matching picks the right face.

  static uint32_t be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }
  static uint32_t be32(const uint8_t* p)
  {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
  }

  static void append_utf8(std::string& out, uint32_t cp)
  {
    if (cp < 0x80) out += static_cast<char>(cp);
    else if (cp < 0x800) { out += char(0xC0 | (cp >> 6)); out += char(0x80 | (cp & 0x3F)); }
    else if (cp < 0x10000) {
      out += char(0xE0 | (cp >> 12)); out += char(0x80 | ((cp >> 6) & 0x3F));
      out += char(0x80 | (cp & 0x3F));
    } else {
      out += char(0xF0 | (cp >> 18)); out += char(0x80 | ((cp >> 12) & 0x3F));
      out += char(0x80 | ((cp >> 6) & 0x3F)); out += char(0x80 | (cp & 0x3F));
    }
  }

  // Family name + usWeightClass of the first face in a TTF / OTF / TTC.
  static bool sfnt_family(const uint8_t* d, uint32_t len, std::string& family, int& weight)
  {
    family.clear();
    weight = 400;
    if (!d || len < 12) return false;
    uint32_t base = 0;
    if (std::memcmp(d, "ttcf", 4) == 0) {
      if (len < 16) return false;
      base = be32(d + 12);   // the first face of the collection
      if (base + 12 > len) return false;
    }
    const uint32_t num = be16(d + base + 4);
    uint32_t name_off = 0, name_len = 0, os2_off = 0, os2_len = 0;
    for (uint32_t i = 0; i < num; ++i) {
      const uint32_t rec = base + 12 + i * 16;
      if (rec + 16 > len) return false;
      const uint32_t off = be32(d + rec + 8), tlen = be32(d + rec + 12);
      if (std::memcmp(d + rec, "name", 4) == 0) { name_off = off; name_len = tlen; }
      if (std::memcmp(d + rec, "OS/2", 4) == 0) { os2_off = off; os2_len = tlen; }
    }
    if (os2_off && os2_len >= 6 && os2_off + 6 <= len) {
      const int w = static_cast<int>(be16(d + os2_off + 4));
      if (w >= 1 && w <= 1000) weight = w;
    }
    if (!name_off || name_off + 6 > len || name_off + name_len > len) return false;
    const uint8_t* nt = d + name_off;
    const uint32_t count = be16(nt + 2), strings = be16(nt + 4);
    // Best record: nameID 16 over 1, Windows Unicode (platform 3) over Mac.
    int best_score = -1;
    uint32_t best_rec = 0;
    for (uint32_t i = 0; i < count; ++i) {
      const uint32_t r = 6 + i * 12;
      if (r + 12 > name_len) break;
      const uint32_t platform = be16(nt + r), name_id = be16(nt + r + 6);
      if (name_id != 1 && name_id != 16) continue;
      if (platform != 3 && platform != 1 && platform != 0) continue;
      const int score = (name_id == 16 ? 4 : 0) + (platform == 3 ? 2 : platform == 0 ? 1 : 0);
      if (score > best_score) { best_score = score; best_rec = r; }
    }
    if (best_score < 0) return false;
    const uint32_t platform = be16(nt + best_rec);
    const uint32_t slen = be16(nt + best_rec + 8), soff = be16(nt + best_rec + 10);
    if (strings + soff + slen > name_len) return false;
    const uint8_t* s = nt + strings + soff;
    if (platform == 1) {   // Mac Roman: keep ASCII
      for (uint32_t i = 0; i < slen; ++i) if (s[i] < 0x80) family += static_cast<char>(s[i]);
    } else {               // UTF-16BE
      for (uint32_t i = 0; i + 1 < slen; i += 2) {
        uint32_t cp = be16(s + i);
        if (cp >= 0xD800 && cp < 0xDC00 && i + 3 < slen) {
          const uint32_t lo = be16(s + i + 2);
          if (lo >= 0xDC00 && lo < 0xE000) { cp = 0x10000 + ((cp - 0xD800) << 10) + (lo - 0xDC00); i += 2; }
        }
        append_utf8(family, cp);
      }
    }
    return !family.empty();
  }

  static void copy_family(const std::string& fam, char* out, uint32_t cap)
  {
    if (!out || cap == 0) return;
    size_t n = fam.size();
    if (n > cap - 1) n = cap - 1;
    std::memcpy(out, fam.data(), n);
    out[n] = '\0';
  }

  static bool c2d_register_font(const uint8_t* data, uint32_t len, char* out_family,
                                uint32_t cap, uint64_t* out_token)
  {
    ensure_setup();
    if (out_family && cap) out_family[0] = '\0';
    if (out_token) *out_token = 0;
    std::string family;
    int weight = 400;
    if (!sfnt_family(data, len, family, weight)) return false;
    const int tok = c2d_font_add(family.c_str(), data, static_cast<int>(len), weight);
    if (!tok) return false;
    measure_cache().clear();
    copy_family(family, out_family, cap);
    if (out_token) *out_token = static_cast<uint64_t>(tok);
    return true;
  }

  static bool c2d_register_font_file(const char* path, char* out_family, uint32_t cap,
                                     uint64_t* out_token)
  {
    if (out_family && cap) out_family[0] = '\0';
    if (out_token) *out_token = 0;
    if (!path || !*path) return false;
    FILE* f = std::fopen(path, "rb");
    if (!f) return false;
    std::vector<uint8_t> bytes;
    uint8_t buf[16384];
    size_t got;
    while ((got = std::fread(buf, 1, sizeof buf, f)) > 0) bytes.insert(bytes.end(), buf, buf + got);
    std::fclose(f);
    if (bytes.empty()) return false;
    return c2d_register_font(bytes.data(), static_cast<uint32_t>(bytes.size()), out_family, cap,
                             out_token);
  }

  static void c2d_unregister_font(uint64_t token)
  {
    if (!token) return;
    c2d_font_remove(static_cast<int>(token));
    measure_cache().clear();
  }

  // --------------------------------------------------------------------------
  // Off-screen contexts (NEUI_ASSET_KIND_SURFACE)

  static neui_render_ctx_t c2d_create_offscreen_context(uint32_t width_px, uint32_t height_px,
                                                        float scale)
  {
    ensure_setup();
    if (width_px == 0 || height_px == 0) return nullptr;
    if (scale <= 0.0f) scale = 1.0f;
    const int id = c2d_ctx_create(0, static_cast<int>(width_px), static_cast<int>(height_px), 1);
    if (!id) return nullptr;
    auto* st = new C2DCtx();
    st->id = id;
    st->is_offscreen = true;
    st->w_px = width_px;
    st->h_px = height_px;
    st->scale = scale;
    return st;
  }

  static bool c2d_read_pixels_bgra(neui_render_ctx_t raw, uint8_t* out_bgra)
  {
    auto* st = static_cast<C2DCtx*>(raw);
    if (!st || !st->is_offscreen || !out_bgra) return false;
    return c2d_read_pixels(st->id, out_bgra, static_cast<int>(st->w_px),
                           static_cast<int>(st->h_px)) != 0;
  }

  // --------------------------------------------------------------------------

  static neui_render_backend_t backend = {
    NEUI_VERSION,
    c2d_create_context,
    c2d_destroy_context,
    c2d_resize,
    c2d_begin_frame,
    c2d_end_frame,
    c2d_fill_rect_fn,
    c2d_draw_rect_fn,
    c2d_get_scale_factor,
    c2d_update_dpi,
    c2d_draw_text,
    c2d_measure_text,
    c2d_push_clip,
    c2d_pop_clip,
    c2d_create_bitmap,
    c2d_destroy_bitmap,
    c2d_draw_bitmap,
    c2d_begin_path,
    c2d_move_to,
    c2d_line_to,
    c2d_arc,
    c2d_close_path,
    c2d_fill_path_fn,
    c2d_stroke_path_fn,
    c2d_push_transform,
    c2d_pop_transform,
    c2d_translate,
    c2d_rotate,
    c2d_scale,
    c2d_get_context_generation,
    c2d_push_alpha,
    c2d_pop_alpha,
    c2d_push_font,
    c2d_pop_font,
    c2d_create_offscreen_context,
    c2d_read_pixels_bgra,
    c2d_register_font,
    c2d_register_font_file,
    c2d_unregister_font,
    c2d_fill_rect_gradient,
    c2d_fill_path_gradient,
    c2d_cubic_to,
    c2d_quad_to,
    c2d_set_fill_rule,
    c2d_stroke_path_styled,
    c2d_stroke_path_gradient,
    c2d_set_frame_damage,
    c2d_get_device_transform,
    // No GPU shader programs: shaders run on the CPU executor and arrive as
    // bitmaps (a WebGL path is deferred, plans/wasm-host.md).
  };

  neui_render_backend_t* get_backend() { return &backend; }

  void set_scale(neui_render_ctx_t ctx, float scale)
  {
    auto* st = static_cast<C2DCtx*>(ctx);
    if (st && scale > 0.0f) st->scale = scale;
  }

  void shutdown()
  {
    c2d_shutdown();
  }

} // namespace neui_canvas2d_backend
