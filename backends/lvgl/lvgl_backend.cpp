// LVGL rendering backend for the crossplatform host (prototype).
//
// Maps neui_render_backend_t onto LVGL v9 draw tasks issued into the layer
// bound by the platform layer around each draw dispatch (bind_layer /
// unbind_layer in lvgl_backend.h):
//
//   fill_rect / draw_rect      -> lv_draw_fill / lv_draw_border
//   draw_text / measure_text   -> lv_draw_label / lv_text_get_size over
//                                 Tiny TTF instances resolved by
//                                 (family, weight, size) from Windows fonts
//   path + gradients           -> the ThorVG vector pipeline
//                                 (lv_draw_vector_*; arcs flattened to
//                                 cubics so sweep semantics match D2D/Cairo)
//   clip stack                 -> save / intersect / restore of the layer's
//                                 _clip_area (draw tasks snapshot it)
//   transform stack            -> software 2x3 CTM; translation/scale take
//                                 the fast integer path, general affines run
//                                 through the vector matrix
//   bitmaps                    -> lv_draw_image over ARGB8888_PREMULTIPLIED
//
// Deferred for the prototype (graceful nulls, per the plan): off-screen
// contexts (SURFACE assets degrade to asset_none), font registration.
//
// IMPORTANT lifetime rule: LVGL draw tasks execute after the dispatch
// returns, so nothing transient may be referenced by a task. Label text is
// copied (text_local = 1), vector paths are deep-copied at add_path, and
// per-draw sub-image descriptors live in a per-ctx arena flushed at the next
// bind (by which time the previous refresh has completed).

#include "lvgl_backend.h"

#include <lvgl/lvgl.h>

#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>

#include <cmath>
#include <cstdio>
#include <cstring>
#include <map>
#include <memory>
#include <string>
#include <vector>

namespace neui_lvgl_backend
{
  // -------------------------------------------------------------------------
  // 2x3 affine CTM (column-vector convention: p' = M * p).

  struct Mat23 {
    float a = 1.0f, b = 0.0f, c = 0.0f, d = 1.0f, tx = 0.0f, ty = 0.0f;
  };

  // r = t o m (apply m first, then t) - post-multiply semantics.
  static Mat23 mat_mul(const Mat23& t, const Mat23& m)
  {
    Mat23 r;
    r.a  = t.a * m.a  + t.c * m.b;
    r.b  = t.b * m.a  + t.d * m.b;
    r.c  = t.a * m.c  + t.c * m.d;
    r.d  = t.b * m.c  + t.d * m.d;
    r.tx = t.a * m.tx + t.c * m.ty + t.tx;
    r.ty = t.b * m.tx + t.d * m.ty + t.ty;
    return r;
  }

  static bool mat_is_axis_aligned(const Mat23& m)
  {
    return std::fabs(m.b) < 1e-4f && std::fabs(m.c) < 1e-4f;
  }

  // -------------------------------------------------------------------------
  // Render context

  struct SubImage;  // per-draw image descriptor arena entry (below)

  struct LvglCtx {
    lv_layer_t* layer = nullptr;
    int32_t     base_x = 0, base_y = 0;
    uint32_t    width = 0, height = 0;   // logical size (frame client area)

    Mat23              tf;
    std::vector<Mat23> tf_stack;

    lv_area_t              bind_clip{};   // layer clip at bind time
    std::vector<lv_area_t> clip_stack;

    std::vector<float> alpha_stack;

    struct FontSel { std::string family; int weight = 0; };
    std::vector<FontSel> font_stack;

    // Path state (raw untransformed coords; CTM applied at fill/stroke time).
    lv_vector_path_t* path = nullptr;
    neui_fill_rule_t  fill_rule = NEUI_FILL_RULE_NONZERO;
    bool  has_current = false;
    float cur_x = 0.0f, cur_y = 0.0f;
    float sub_start_x = 0.0f, sub_start_y = 0.0f;

    // Vector-task batch: consecutive fill/stroke path ops accumulate into ONE
    // lv_draw_vector task, flushed at the next non-path draw / clip change /
    // unbind. Matters enormously on non-32bpp targets, where LVGL's SW vector
    // fallback pays a full-layer ARGB8888 round-trip PER TASK (a KNOB is ~6
    // path ops -> 1 round-trip instead of 6).
    lv_draw_vector_dsc_t* batch = nullptr;

    std::vector<std::unique_ptr<SubImage>> subimage_arena;

    // NOTE: does NOT touch subimage_arena - pending LVGL draw tasks may
    // still reference those descriptors (retained mode re-binds per widget
    // within one refresh). collect_deferred() frees them post-refresh.
    void reset_paint_state()
    {
      tf = Mat23{};
      tf_stack.clear();
      clip_stack.clear();
      alpha_stack.clear();
      font_stack.clear();
    }
  };

  static float current_alpha(LvglCtx* c)
  {
    float a = 1.0f;
    for (float f : c->alpha_stack) a *= f;
    return a;
  }

  static lv_color_t to_color(uint32_t argb)
  {
    return lv_color_make(static_cast<uint8_t>(argb >> 16),
                         static_cast<uint8_t>(argb >> 8),
                         static_cast<uint8_t>(argb));
  }

  static lv_opa_t fold_opa(LvglCtx* c, uint32_t argb)
  {
    float a = static_cast<float>(argb >> 24) * current_alpha(c);
    if (a < 0.0f) a = 0.0f;
    if (a > 255.0f) a = 255.0f;
    return static_cast<lv_opa_t>(a + 0.5f);
  }

  // Map a logical point through the CTM + base offset into display coords.
  static void map_point(LvglCtx* c, float x, float y, float* ox, float* oy)
  {
    *ox = static_cast<float>(c->base_x) + c->tf.a * x + c->tf.c * y + c->tf.tx;
    *oy = static_cast<float>(c->base_y) + c->tf.b * x + c->tf.d * y + c->tf.ty;
  }

  // Map an axis-aligned logical rect to a display-coord lv_area_t (AABB of
  // the four mapped corners, so it stays correct under any CTM - callers on
  // the fast paths only use it when the CTM is axis-aligned).
  static lv_area_t map_rect(LvglCtx* c, float x, float y, float w, float h)
  {
    float x0, y0, x1, y1, x2, y2, x3, y3;
    map_point(c, x,     y,     &x0, &y0);
    map_point(c, x + w, y,     &x1, &y1);
    map_point(c, x,     y + h, &x2, &y2);
    map_point(c, x + w, y + h, &x3, &y3);
    float minx = std::fmin(std::fmin(x0, x1), std::fmin(x2, x3));
    float miny = std::fmin(std::fmin(y0, y1), std::fmin(y2, y3));
    float maxx = std::fmax(std::fmax(x0, x1), std::fmax(x2, x3));
    float maxy = std::fmax(std::fmax(y0, y1), std::fmax(y2, y3));
    lv_area_t a;
    a.x1 = static_cast<int32_t>(std::lround(minx));
    a.y1 = static_cast<int32_t>(std::lround(miny));
    a.x2 = static_cast<int32_t>(std::lround(maxx)) - 1;
    a.y2 = static_cast<int32_t>(std::lround(maxy)) - 1;
    return a;
  }

  static lv_area_t intersect(const lv_area_t& a, const lv_area_t& b)
  {
    lv_area_t r = a;
    if (b.x1 > r.x1) r.x1 = b.x1;
    if (b.y1 > r.y1) r.y1 = b.y1;
    if (b.x2 < r.x2) r.x2 = b.x2;
    if (b.y2 < r.y2) r.y2 = b.y2;
    return r;  // may be degenerate (x2 < x1) - nothing draws then
  }

  // Submit + drop the pending batched vector task, if any. Must run before
  // any non-vector draw (z-order), any clip change (tasks snapshot the
  // layer's clip at submission), end of frame, and unbind.
  static void flush_vector_batch(LvglCtx* c)
  {
    if (!c->batch) return;
    lv_draw_vector(c->batch);
    lv_draw_vector_dsc_delete(c->batch);
    c->batch = nullptr;
  }

  // The batch descriptor, created lazily on the first path op of a run.
  // Per-path state (transform, fill/stroke) is set before each add_path;
  // the caller must set BOTH halves every time since descriptor state
  // persists across add_path calls within one batch.
  static lv_draw_vector_dsc_t* vector_batch(LvglCtx* c)
  {
    if (!c->batch) c->batch = lv_draw_vector_dsc_create(c->layer);
    return c->batch;
  }

  // Reset stroke style state to the plain-stroke defaults (a previous
  // batched path may have left caps / joins / dashes behind).
  static void reset_stroke_style(lv_draw_vector_dsc_t* v)
  {
    lv_draw_vector_dsc_set_stroke_cap(v, LV_VECTOR_STROKE_CAP_BUTT);
    lv_draw_vector_dsc_set_stroke_join(v, LV_VECTOR_STROKE_JOIN_MITER);
    lv_draw_vector_dsc_set_stroke_miter_limit(v, 4);
    lv_draw_vector_dsc_set_stroke_dash(v, nullptr, 0);
  }

  // The full CTM + base offset as an lv_matrix_t for the vector pipeline.
  static lv_matrix_t vector_matrix(LvglCtx* c)
  {
    lv_matrix_t m;
    m.m[0][0] = c->tf.a;  m.m[0][1] = c->tf.c;  m.m[0][2] = c->tf.tx + static_cast<float>(c->base_x);
    m.m[1][0] = c->tf.b;  m.m[1][1] = c->tf.d;  m.m[1][2] = c->tf.ty + static_cast<float>(c->base_y);
    m.m[2][0] = 0.0f;     m.m[2][1] = 0.0f;     m.m[2][2] = 1.0f;
    return m;
  }

  // -------------------------------------------------------------------------
  // LVGL lock hooks (see lvgl_backend.h). Installed by the platform layer;
  // absent, the guard is a no-op.

  static void (*g_lv_lock)()   = nullptr;
  static void (*g_lv_unlock)() = nullptr;

  void set_lock_hooks(void (*lock)(), void (*unlock)())
  {
    g_lv_lock   = lock;
    g_lv_unlock = unlock;
  }

  struct LvGuard {
    LvGuard()  { if (g_lv_lock)   g_lv_lock();   }
    ~LvGuard() { if (g_lv_unlock) g_lv_unlock(); }
    LvGuard(const LvGuard&) = delete;
    LvGuard& operator=(const LvGuard&) = delete;
  };

  // -------------------------------------------------------------------------
  // Fonts: (family, weight, size) -> Tiny TTF instance over a Windows font
  // file. Factory-level caches; every call is on the UI thread.

  struct FontFile {
    std::vector<uint8_t> bytes;
  };

  static std::map<std::string, std::shared_ptr<FontFile>>& font_files()
  {
    static std::map<std::string, std::shared_ptr<FontFile>> m;
    return m;
  }

  struct FontInstanceKey {
    const FontFile* file;
    int             size_px;
    bool operator<(const FontInstanceKey& o) const
    {
      if (file != o.file) return file < o.file;
      return size_px < o.size_px;
    }
  };

  static std::map<FontInstanceKey, lv_font_t*>& font_instances()
  {
    static std::map<FontInstanceKey, lv_font_t*> m;
    return m;
  }

  // Front cache keyed on what the caller actually passes. The file-resolution
  // path below builds strings and touches the filesystem, which is far too
  // expensive for a hot paint path (an INPUTBOX repaint alone issues one
  // measure_text per caret / selection query, a MULTILINE one per visual row).
  struct FontLookupKey {
    std::string family;
    int         weight;
    int         size_px;
    bool operator<(const FontLookupKey& o) const
    {
      if (size_px != o.size_px) return size_px < o.size_px;
      if (weight != o.weight) return weight < o.weight;
      return family < o.family;
    }
  };

  static std::map<FontLookupKey, const lv_font_t*>& font_lookup()
  {
    static std::map<FontLookupKey, const lv_font_t*> m;
    return m;
  }

  static std::string lower_nospace(const char* s)
  {
    std::string out;
    for (; s && *s; ++s) {
      char ch = *s;
      if (ch == ' ') continue;
      if (ch >= 'A' && ch <= 'Z') ch = static_cast<char>(ch - 'A' + 'a');
      out.push_back(ch);
    }
    return out;
  }

  // Resolved once - draw_text / measure_text run per glyph run and per caret
  // query, so nothing on that path may issue a syscall or build a path string.
  static const std::string& windows_fonts_dir()
  {
    static const std::string dir = [] {
      char buf[MAX_PATH] = {};
      UINT n = GetWindowsDirectoryA(buf, MAX_PATH);
      std::string d = (n > 0 && n < MAX_PATH) ? std::string(buf) : "C:\\Windows";
      return d + "\\Fonts\\";
    }();
    return dir;
  }

  static std::shared_ptr<FontFile> load_font_file(const std::string& path)
  {
    auto it = font_files().find(path);
    if (it != font_files().end()) return it->second;

    std::shared_ptr<FontFile> ff;
    FILE* f = nullptr;
    if (fopen_s(&f, path.c_str(), "rb") == 0 && f) {
      fseek(f, 0, SEEK_END);
      long sz = ftell(f);
      fseek(f, 0, SEEK_SET);
      if (sz > 0) {
        ff = std::make_shared<FontFile>();
        ff->bytes.resize(static_cast<size_t>(sz));
        if (fread(ff->bytes.data(), 1, static_cast<size_t>(sz), f) !=
            static_cast<size_t>(sz))
          ff.reset();
      }
      fclose(f);
    }
    font_files()[path] = ff;  // negative results cached too
    return ff;
  }

  // Resolve (family, weight) to a loaded font file. Well-known Windows
  // families get their real file names; anything else tries
  // "<family-lowercase-no-spaces>.ttf" and falls back to Segoe UI.
  static std::shared_ptr<FontFile> resolve_font_file(const char* family, int weight)
  {
    const bool bold = weight >= 600;
    const std::string& dir = windows_fonts_dir();
    std::string fam = lower_nospace(family);

    struct Known { const char* fam; const char* normal; const char* bold; };
    static const Known known[] = {
      { "",              "segoeui.ttf", "segoeuib.ttf" },
      { "segoeui",       "segoeui.ttf", "segoeuib.ttf" },
      { "arial",         "arial.ttf",   "arialbd.ttf"  },
      { "consolas",      "consola.ttf", "consolab.ttf" },
      { "tahoma",        "tahoma.ttf",  "tahomabd.ttf" },
      { "verdana",       "verdana.ttf", "verdanab.ttf" },
      { "couriernew",    "cour.ttf",    "courbd.ttf"   },
      { "timesnewroman", "times.ttf",   "timesbd.ttf"  },
    };
    for (const auto& k : known) {
      if (fam == k.fam) {
        if (auto ff = load_font_file(dir + (bold ? k.bold : k.normal))) return ff;
        break;
      }
    }
    if (!fam.empty()) {
      if (auto ff = load_font_file(dir + fam + (bold ? "b.ttf" : ".ttf"))) return ff;
      if (auto ff = load_font_file(dir + fam + ".ttf")) return ff;
    }
    if (auto ff = load_font_file(dir + (bold ? "segoeuib.ttf" : "segoeui.ttf")))
      return ff;
    return load_font_file(dir + "segoeui.ttf");
  }

  // `c` may be null: measure_text is called from the host's non-painting
  // sizing paths (ComboBoxWidget::drop_width, popup_total_width), which have no
  // bound context. Those get the default selection (family "", weight 0) and an
  // identity CTM - the same thing d2d_measure_text does with a null ctx.
  static const lv_font_t* resolve_font(LvglCtx* c, float font_size)
  {
    const char* family = "";
    int weight = 0;
    if (c && !c->font_stack.empty()) {
      family = c->font_stack.back().family.c_str();
      weight = c->font_stack.back().weight;
    }

    // Fold any CTM scale into the pixel size (rare; text is normally drawn
    // under translation-only transforms).
    const float scale = (c && mat_is_axis_aligned(c->tf)) ? std::fabs(c->tf.d) : 1.0f;
    int size_px = static_cast<int>(std::lround(font_size * scale));
    if (size_px < 1) size_px = 1;

    FontLookupKey key{ family, weight, size_px };
    auto lit = font_lookup().find(key);
    if (lit != font_lookup().end()) return lit->second;

    const lv_font_t* result = nullptr;
    if (auto ff = resolve_font_file(family, weight)) {
      FontInstanceKey ikey{ ff.get(), size_px };
      auto it = font_instances().find(ikey);
      if (it != font_instances().end()) {
        result = it->second;
      } else {
        // Allocates through LVGL and registers glyph caches - guard it, this
        // runs on the first use of a (font, size) pair, which can be a click
        // handler outside any draw dispatch.
        LvGuard guard;
        lv_font_t* font = lv_tiny_ttf_create_data(
            ff->bytes.data(), ff->bytes.size(), size_px);
        font_instances()[ikey] = font;  // null cached too (falls back below)
        result = font;
      }
    }
    if (!result) result = lv_font_get_default();
    font_lookup()[key] = result;
    return result;
  }

  // -------------------------------------------------------------------------
  // Context lifecycle. native_handle is unused (the platform layer owns the
  // lv_display); a ctx is just the paint-state bundle bound to layers later.

  static neui_render_ctx_t NEUI_ABI lb_create_context(void*, uint32_t w, uint32_t h)
  {
    auto* c = new LvglCtx();
    c->width  = w;
    c->height = h;
    return c;
  }

  static void NEUI_ABI lb_destroy_context(neui_render_ctx_t raw)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c) return;
    {
      // Window teardown, not a draw dispatch: these free through LVGL's
      // allocator, which a display thread can be inside under lv_lock.
      LvGuard guard;
      if (c->batch) lv_draw_vector_dsc_delete(c->batch);
      if (c->path) lv_vector_path_delete(c->path);
    }
    delete c;
  }

  static void NEUI_ABI lb_resize(neui_render_ctx_t raw, uint32_t w, uint32_t h)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c) return;
    c->width  = w;
    c->height = h;
  }

  void bind_layer(neui_render_ctx_t raw, _lv_layer_t* layer,
                  int32_t base_x, int32_t base_y)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c) return;
    if (c->batch) {
      // Defensive: a leftover batch references a stale layer - drop it.
      lv_draw_vector_dsc_delete(c->batch);
      c->batch = nullptr;
    }
    c->layer  = layer;
    c->base_x = base_x;
    c->base_y = base_y;
    c->reset_paint_state();
    if (layer) c->bind_clip = layer->_clip_area;
  }

  void unbind_layer(neui_render_ctx_t raw)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c) return;
    flush_vector_batch(c);   // before the clip restore - tasks snapshot it
    if (c->layer) c->layer->_clip_area = c->bind_clip;
    c->layer = nullptr;
    c->clip_stack.clear();
  }

  void collect_deferred(neui_render_ctx_t raw)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (c) c->subimage_arena.clear();
  }

  // -------------------------------------------------------------------------
  // Frame + solid shapes

  static void NEUI_ABI lb_fill_rect(neui_render_ctx_t raw,
                                    float x, float y, float w, float h,
                                    uint32_t argb);

  static void NEUI_ABI lb_begin_frame(neui_render_ctx_t raw, uint32_t clear_argb)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->layer) return;
    // Stacks reset per contract. (bind_layer already did this, but paint_frame
    // may begin several logical frames on one binding in principle.)
    lv_area_t keep_clip = c->layer->_clip_area;
    c->reset_paint_state();
    c->layer->_clip_area = keep_clip;
    lb_fill_rect(raw, 0.0f, 0.0f,
                 static_cast<float>(c->width), static_cast<float>(c->height),
                 clear_argb | 0xFF000000u);
  }

  static void NEUI_ABI lb_end_frame(neui_render_ctx_t raw)
  {
    // Presentation is LVGL's job (flush_cb after the refresh); just make sure
    // no batched vector task is left pending.
    if (auto* c = static_cast<LvglCtx*>(raw)) flush_vector_batch(c);
  }

  // Fill an arbitrary quad via the vector pipeline (general-affine fallback).
  static void fill_rect_vector(LvglCtx* c, float x, float y, float w, float h,
                               uint32_t argb)
  {
    lv_draw_vector_dsc_t* v = vector_batch(c);
    if (!v) return;
    lv_vector_path_t* p = lv_vector_path_create(LV_VECTOR_PATH_QUALITY_MEDIUM);
    lv_vector_path_append_rectangle(p, x, y, w, h, 0.0f, 0.0f);
    lv_matrix_t m = vector_matrix(c);
    lv_draw_vector_dsc_set_transform(v, &m);
    lv_draw_vector_dsc_set_fill_color(v, to_color(argb));
    lv_draw_vector_dsc_set_fill_opa(v, fold_opa(c, argb));
    lv_draw_vector_dsc_set_fill_rule(v, LV_VECTOR_FILL_NONZERO);
    lv_draw_vector_dsc_set_stroke_opa(v, LV_OPA_TRANSP);
    lv_draw_vector_dsc_add_path(v, p);
    lv_vector_path_delete(p);
  }

  static void NEUI_ABI lb_fill_rect(neui_render_ctx_t raw,
                                    float x, float y, float w, float h,
                                    uint32_t argb)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->layer || w <= 0.0f || h <= 0.0f) return;
    if (!mat_is_axis_aligned(c->tf)) { fill_rect_vector(c, x, y, w, h, argb); return; }
    flush_vector_batch(c);   // keep z-order vs the lv_draw_fill task below

    lv_draw_fill_dsc_t dsc;
    lv_draw_fill_dsc_init(&dsc);
    dsc.color  = to_color(argb);
    dsc.opa    = fold_opa(c, argb);
    dsc.radius = 0;
    lv_area_t a = map_rect(c, x, y, w, h);
    lv_draw_fill(c->layer, &dsc, &a);
  }

  static void NEUI_ABI lb_draw_rect(neui_render_ctx_t raw,
                                    float x, float y, float w, float h,
                                    float stroke_width, uint32_t argb)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->layer || w <= 0.0f || h <= 0.0f) return;

    if (!mat_is_axis_aligned(c->tf)) {
      // General affine: stroke the rect as a vector path.
      lv_draw_vector_dsc_t* v = vector_batch(c);
      if (!v) return;
      lv_vector_path_t* p = lv_vector_path_create(LV_VECTOR_PATH_QUALITY_MEDIUM);
      lv_vector_path_append_rectangle(p, x, y, w, h, 0.0f, 0.0f);
      lv_matrix_t m = vector_matrix(c);
      lv_draw_vector_dsc_set_transform(v, &m);
      lv_draw_vector_dsc_set_fill_opa(v, LV_OPA_TRANSP);
      lv_draw_vector_dsc_set_stroke_color(v, to_color(argb));
      lv_draw_vector_dsc_set_stroke_opa(v, fold_opa(c, argb));
      lv_draw_vector_dsc_set_stroke_width(v, stroke_width);
      reset_stroke_style(v);
      lv_draw_vector_dsc_add_path(v, p);
      lv_vector_path_delete(p);
      return;
    }
    flush_vector_batch(c);   // lv_draw_border below must stay in z-order

    // D2D's DrawRectangle strokes CENTERED on the rect edge; lv_draw_border
    // draws inside its area. Expand by half the stroke so the border
    // straddles the edge the same way.
    lv_draw_border_dsc_t dsc;
    lv_draw_border_dsc_init(&dsc);
    dsc.color  = to_color(argb);
    dsc.opa    = fold_opa(c, argb);
    dsc.width  = static_cast<int32_t>(std::lround(stroke_width < 1.0f ? 1.0f : stroke_width));
    dsc.radius = 0;
    dsc.side   = LV_BORDER_SIDE_FULL;
    const float half = stroke_width * 0.5f;
    lv_area_t a = map_rect(c, x - half, y - half, w + stroke_width, h + stroke_width);
    lv_draw_border(c->layer, &dsc, &a);
  }

  static float NEUI_ABI lb_get_scale_factor(neui_render_ctx_t)
  {
    return 1.0f;  // fixed-scale prototype (plan: get_scale_factor may be fixed)
  }

  static void NEUI_ABI lb_update_dpi(neui_render_ctx_t, uint32_t) {}

  // -------------------------------------------------------------------------
  // Text

  static void NEUI_ABI lb_draw_text(neui_render_ctx_t raw,
                                    float x, float y, float w, float h,
                                    const char* text, float font_size,
                                    uint32_t argb)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->layer || !text || !*text || w <= 0.0f || h <= 0.0f) return;

    flush_vector_batch(c);   // text must draw above earlier batched paths

    const lv_font_t* font = resolve_font(c, font_size);
    lv_draw_label_dsc_t dsc;
    lv_draw_label_dsc_init(&dsc);
    dsc.text       = text;
    dsc.text_local = 1;   // tasks run later - LVGL must copy the string
    dsc.font       = font;
    dsc.color      = to_color(argb);
    dsc.opa        = fold_opa(c, argb);
    // NO wrap, for parity with every other backend: D2D sets
    // DWRITE_WORD_WRAPPING_NO_WRAP, Cairo / CG break only on an explicit '\n'.
    // Overflow clips at the rect edge (bracket below). Without this LVGL would
    // word-wrap at the rect width, which also breaks the single-line caret /
    // selection math the text widgets do against measure_text.
    dsc.flag = LV_TEXT_FLAG_EXPAND;

    lv_area_t a = map_rect(c, x, y, w, h);

    // Vertical centering of the text block inside the rect, matching D2D
    // (DWRITE_PARAGRAPH_ALIGNMENT_CENTER), Cairo and CG. Widget paints pass the
    // full widget rect and rely on it. LVGL always draws from coords.y1, so
    // position a tight measured box instead: EXPAND makes the label ignore the
    // box width for line breaking, so the box stays tight (cheap draw-task
    // dependency bookkeeping) while the text still never wraps.
    lv_point_t block{};
    lv_text_get_size(&block, text, font, dsc.letter_space, dsc.line_space,
                     LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    const int32_t box_h = a.y2 - a.y1 + 1;
    lv_area_t la = a;
    la.y1 = a.y1 + static_cast<int32_t>(
                       std::lround((box_h - block.y) * 0.5f));
    la.y2 = la.y1 + (block.y > 0 ? block.y : 1) - 1;
    la.x2 = la.x1 + (block.x > 0 ? block.x : 1) - 1;

    lv_area_t saved = c->layer->_clip_area;
    c->layer->_clip_area = intersect(saved, a);
    lv_draw_label(c->layer, &dsc, &la);
    c->layer->_clip_area = saved;
  }

  static float NEUI_ABI lb_measure_text(neui_render_ctx_t raw,
                                        const char* text, int text_len,
                                        float font_size)
  {
    // NOTE: raw may legitimately be null - the host's non-painting sizing paths
    // (ComboBoxWidget::drop_width, popup_total_width) measure without a bound
    // context, exactly as they do against d2d_measure_text. Returning 0 there
    // collapses combo drop widths and popup menu widths to their minimums.
    auto* c = static_cast<LvglCtx*>(raw);
    if (!text || !*text) return 0.0f;

    // Measuring fills the Tiny TTF glyph cache, i.e. mutates LVGL state - and
    // this is reachable from the host's input handlers, not just from paint.
    LvGuard guard;
    const lv_font_t* font = resolve_font(c, font_size);
    if (!font) return 0.0f;

    // lv_text_get_size wants a NUL-terminated string; honour the byte limit.
    std::string tmp;
    const char* s = text;
    if (text_len >= 0) {
      tmp.assign(text, static_cast<size_t>(text_len));
      s = tmp.c_str();
    }
    lv_point_t size{};
    lv_text_get_size(&size, s, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return static_cast<float>(size.x);
  }

  // -------------------------------------------------------------------------
  // Clip stack

  static void NEUI_ABI lb_push_clip(neui_render_ctx_t raw,
                                    float x, float y, float w, float h)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->layer) return;
    flush_vector_batch(c);   // tasks snapshot the clip at submission
    c->clip_stack.push_back(c->layer->_clip_area);
    lv_area_t want = map_rect(c, x, y, w, h);
    c->layer->_clip_area = intersect(c->layer->_clip_area, want);
  }

  static void NEUI_ABI lb_pop_clip(neui_render_ctx_t raw)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->layer || c->clip_stack.empty()) return;
    flush_vector_batch(c);   // tasks snapshot the clip at submission
    c->layer->_clip_area = c->clip_stack.back();
    c->clip_stack.pop_back();
  }

  // -------------------------------------------------------------------------
  // Bitmaps. Handles own a premultiplied-ARGB8888 lv_image_dsc_t; sub-rect
  // draws allocate a per-draw descriptor in the ctx arena (freed at the next
  // bind, after the refresh that consumed it has finished).

  struct LvglBitmap {
    lv_image_dsc_t       dsc{};
    std::vector<uint8_t> pixels;
    float                scale = 1.0f;
  };

  struct SubImage {
    lv_image_dsc_t dsc{};
  };

  static void* NEUI_ABI lb_create_bitmap(neui_render_ctx_t,
                                         uint32_t width_px, uint32_t height_px,
                                         const uint8_t* bgra, float scale)
  {
    if (!bgra || width_px == 0 || height_px == 0) return nullptr;
    auto* bmp = new LvglBitmap();
    bmp->pixels.assign(bgra, bgra + static_cast<size_t>(width_px) * height_px * 4);
    bmp->scale = (scale > 0.0f) ? scale : 1.0f;
    bmp->dsc.header.magic = LV_IMAGE_HEADER_MAGIC;
    bmp->dsc.header.cf     = LV_COLOR_FORMAT_ARGB8888_PREMULTIPLIED;
    bmp->dsc.header.w      = width_px;
    bmp->dsc.header.h      = height_px;
    bmp->dsc.header.stride = width_px * 4;
    bmp->dsc.data          = bmp->pixels.data();
    bmp->dsc.data_size     = static_cast<uint32_t>(bmp->pixels.size());
    return bmp;
  }

  static void NEUI_ABI lb_destroy_bitmap(neui_render_ctx_t, void* bitmap)
  {
    delete static_cast<LvglBitmap*>(bitmap);
  }

  static void NEUI_ABI lb_draw_bitmap(neui_render_ctx_t raw, void* bitmap,
                                      float src_x, float src_y,
                                      float src_w, float src_h,
                                      float dst_x, float dst_y,
                                      float dst_w, float dst_h,
                                      uint32_t tint)
  {
    auto* c   = static_cast<LvglCtx*>(raw);
    auto* bmp = static_cast<LvglBitmap*>(bitmap);
    if (!c || !c->layer || !bmp || dst_w <= 0.0f || dst_h <= 0.0f) return;
    flush_vector_batch(c);   // keep z-order vs the image task below

    // Source rect: logical -> physical pixels of the bitmap.
    const float s = bmp->scale;
    int32_t sx = static_cast<int32_t>(std::lround(src_x * s));
    int32_t sy = static_cast<int32_t>(std::lround(src_y * s));
    int32_t sw = static_cast<int32_t>(std::lround(src_w * s));
    int32_t sh = static_cast<int32_t>(std::lround(src_h * s));
    if (sw <= 0 || sh <= 0) { sx = 0; sy = 0; sw = static_cast<int32_t>(bmp->dsc.header.w); sh = static_cast<int32_t>(bmp->dsc.header.h); }
    if (sx < 0) sx = 0;
    if (sy < 0) sy = 0;
    if (sx + sw > static_cast<int32_t>(bmp->dsc.header.w)) sw = static_cast<int32_t>(bmp->dsc.header.w) - sx;
    if (sy + sh > static_cast<int32_t>(bmp->dsc.header.h)) sh = static_cast<int32_t>(bmp->dsc.header.h) - sy;
    if (sw <= 0 || sh <= 0) return;

    // Sub-image descriptor pointing into the owning bitmap's pixels (stride
    // stays the full row). Arena-owned: must outlive the deferred draw task.
    auto sub = std::make_unique<SubImage>();
    sub->dsc = bmp->dsc;
    sub->dsc.header.w = static_cast<uint32_t>(sw);
    sub->dsc.header.h = static_cast<uint32_t>(sh);
    sub->dsc.data     = bmp->pixels.data()
                      + static_cast<size_t>(sy) * bmp->dsc.header.stride
                      + static_cast<size_t>(sx) * 4;

    lv_draw_image_dsc_t dsc;
    lv_draw_image_dsc_init(&dsc);
    dsc.src   = &sub->dsc;
    // `tint` is a multiplicative ARGB (renderer.h): its alpha scales the draw
    // opacity on top of the ctx alpha stack, its RGB multiplies the pixels.
    // d2d/cg fold both into a colour-matrix; cairo paints at alpha then
    // MULTIPLYs the RGB through the image's own alpha mask.
    dsc.opa = fold_opa(c, tint);
    if ((tint & 0x00FFFFFFu) != 0x00FFFFFFu) {
      // LVGL offers no multiply - only recolor, a mix toward a flat colour. At
      // full strength that reproduces the multiply exactly for the dominant
      // case (a white / greyscale glyph or icon colourised by the tint RGB).
      // A darkening grey tint over already-coloured pixels flattens rather than
      // scaling; accepted prototype approximation. Driving recolor_opa from the
      // ALPHA byte, as this did, is wrong twice over: it made a translucent
      // tint fully opaque and turned "no colour change" into a 50% white wash.
      dsc.recolor     = to_color(tint);
      dsc.recolor_opa = LV_OPA_COVER;
    }
    // Place the image's natural (physical-px) extent at the dst origin and
    // scale it around the top-left pivot onto the dst rect. Fold in any
    // axis-aligned CTM scale via the mapped dst rect.
    lv_area_t dst = map_rect(c, dst_x, dst_y, dst_w, dst_h);
    const float dst_wpx = static_cast<float>(dst.x2 - dst.x1 + 1);
    const float dst_hpx = static_cast<float>(dst.y2 - dst.y1 + 1);
    dsc.scale_x = static_cast<int32_t>(std::lround(256.0f * dst_wpx / static_cast<float>(sw)));
    dsc.scale_y = static_cast<int32_t>(std::lround(256.0f * dst_hpx / static_cast<float>(sh)));
    dsc.pivot.x = 0;
    dsc.pivot.y = 0;

    lv_area_t coords;
    coords.x1 = dst.x1;
    coords.y1 = dst.y1;
    coords.x2 = dst.x1 + sw - 1;
    coords.y2 = dst.y1 + sh - 1;

    // Keep the transformed output inside the dst rect.
    lv_area_t saved = c->layer->_clip_area;
    c->layer->_clip_area = intersect(saved, dst);
    lv_draw_image(c->layer, &dsc, &coords);
    c->layer->_clip_area = saved;

    c->subimage_arena.push_back(std::move(sub));
  }

  // -------------------------------------------------------------------------
  // Path API. Verbs collect raw logical coords; the CTM is applied at fill /
  // stroke time through the vector matrix (matches D2D, where the target
  // transform applies when the geometry is drawn).

  static void NEUI_ABI lb_begin_path(neui_render_ctx_t raw)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c) return;
    if (c->path) lv_vector_path_clear(c->path);
    else         c->path = lv_vector_path_create(LV_VECTOR_PATH_QUALITY_MEDIUM);
    c->fill_rule   = NEUI_FILL_RULE_NONZERO;
    c->has_current = false;
  }

  static void NEUI_ABI lb_move_to(neui_render_ctx_t raw, float x, float y)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->path) return;
    lv_fpoint_t p{ x, y };
    lv_vector_path_move_to(c->path, &p);
    c->has_current = true;
    c->cur_x = x; c->cur_y = y;
    c->sub_start_x = x; c->sub_start_y = y;
  }

  static void NEUI_ABI lb_line_to(neui_render_ctx_t raw, float x, float y)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->path) return;
    if (!c->has_current) { lb_move_to(raw, x, y); return; }
    lv_fpoint_t p{ x, y };
    lv_vector_path_line_to(c->path, &p);
    c->cur_x = x; c->cur_y = y;
  }

  static void NEUI_ABI lb_cubic_to(neui_render_ctx_t raw, float c1x, float c1y,
                                   float c2x, float c2y, float x, float y)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->path) return;
    if (!c->has_current) lb_move_to(raw, c1x, c1y);
    lv_fpoint_t p1{ c1x, c1y }, p2{ c2x, c2y }, p3{ x, y };
    lv_vector_path_cubic_to(c->path, &p1, &p2, &p3);
    c->cur_x = x; c->cur_y = y;
  }

  static void NEUI_ABI lb_quad_to(neui_render_ctx_t raw, float cx, float cy,
                                  float x, float y)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->path) return;
    if (!c->has_current) lb_move_to(raw, cx, cy);
    lv_fpoint_t p1{ cx, cy }, p2{ x, y };
    lv_vector_path_quad_to(c->path, &p1, &p2);
    c->cur_x = x; c->cur_y = y;
  }

  // Append a centre-parameterised arc as cubic segments. Sweep direction is
  // implicit from start -> end (Y-down, positive sweep = clockwise on
  // screen), matching the D2D / CG / Cairo backends: an existing current
  // point connects to the arc start with a line (Cairo semantics).
  static void NEUI_ABI lb_arc(neui_render_ctx_t raw,
                              float cx, float cy, float radius,
                              float start_rad, float end_rad)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->path || radius <= 0.0f) return;

    const float sx = cx + radius * std::cos(start_rad);
    const float sy = cy + radius * std::sin(start_rad);
    if (c->has_current) lb_line_to(raw, sx, sy);
    else                lb_move_to(raw, sx, sy);

    float sweep = end_rad - start_rad;
    if (sweep == 0.0f) return;
    const int   nseg = static_cast<int>(std::ceil(std::fabs(sweep) / (3.14159265f / 2.0f)));
    const float step = sweep / static_cast<float>(nseg);
    const float k    = 4.0f / 3.0f * std::tan(step / 4.0f);

    float a0 = start_rad;
    for (int i = 0; i < nseg; ++i) {
      const float a1  = a0 + step;
      const float c0x = std::cos(a0), c0y = std::sin(a0);
      const float c1x = std::cos(a1), c1y = std::sin(a1);
      lv_fpoint_t p1{ cx + radius * (c0x - k * c0y), cy + radius * (c0y + k * c0x) };
      lv_fpoint_t p2{ cx + radius * (c1x + k * c1y), cy + radius * (c1y - k * c1x) };
      lv_fpoint_t p3{ cx + radius * c1x,             cy + radius * c1y };
      lv_vector_path_cubic_to(c->path, &p1, &p2, &p3);
      a0 = a1;
    }
    c->cur_x = cx + radius * std::cos(end_rad);
    c->cur_y = cy + radius * std::sin(end_rad);
  }

  static void NEUI_ABI lb_close_path(neui_render_ctx_t raw)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->path) return;
    lv_vector_path_close(c->path);
    c->cur_x = c->sub_start_x;
    c->cur_y = c->sub_start_y;
  }

  static void NEUI_ABI lb_set_fill_rule(neui_render_ctx_t raw, neui_fill_rule_t rule)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c) return;
    c->fill_rule = rule;
  }

  static void apply_stroke_style(lv_draw_vector_dsc_t* v,
                                 const neui_stroke_style_t* style)
  {
    if (!style) return;
    switch (style->cap) {
      case NEUI_LINE_CAP_ROUND:  lv_draw_vector_dsc_set_stroke_cap(v, LV_VECTOR_STROKE_CAP_ROUND);  break;
      case NEUI_LINE_CAP_SQUARE: lv_draw_vector_dsc_set_stroke_cap(v, LV_VECTOR_STROKE_CAP_SQUARE); break;
      default:                   lv_draw_vector_dsc_set_stroke_cap(v, LV_VECTOR_STROKE_CAP_BUTT);   break;
    }
    switch (style->join) {
      case NEUI_LINE_JOIN_ROUND: lv_draw_vector_dsc_set_stroke_join(v, LV_VECTOR_STROKE_JOIN_ROUND); break;
      case NEUI_LINE_JOIN_BEVEL: lv_draw_vector_dsc_set_stroke_join(v, LV_VECTOR_STROKE_JOIN_BEVEL); break;
      default:                   lv_draw_vector_dsc_set_stroke_join(v, LV_VECTOR_STROKE_JOIN_MITER); break;
    }
    const float miter = style->miter_limit > 0.0f ? style->miter_limit : 4.0f;
    lv_draw_vector_dsc_set_stroke_miter_limit(v, static_cast<uint16_t>(miter));
    if (style->dash_array && style->dash_count > 0) {
      // (dash_offset is not expressible through the LVGL vector API - the
      // pattern starts at the path start; acceptable for the prototype.)
      std::vector<float> dashes(style->dash_array,
                                style->dash_array + style->dash_count);
      lv_draw_vector_dsc_set_stroke_dash(v, dashes.data(),
                                         static_cast<uint16_t>(dashes.size()));
    }
  }

  static void set_vector_gradient(lv_draw_vector_dsc_t* v,
                                  const neui_gradient_t* grad,
                                  LvglCtx* c, bool stroke)
  {
    // lv_conf.h raises LV_GRADIENT_MAX_STOPS to 16 for us, but neui's stop list
    // is unbounded - clamp here rather than letting LVGL truncate with a
    // per-frame LV_LOG_WARN on stdout.
    uint32_t stop_count = grad->stop_count;
    if (stop_count > LV_GRADIENT_MAX_STOPS) stop_count = LV_GRADIENT_MAX_STOPS;
    std::vector<lv_grad_stop_t> stops(stop_count);
    for (uint32_t i = 0; i < stop_count; ++i) {
      const auto& s = grad->stops[i];
      float off = s.offset;
      if (off < 0.0f) off = 0.0f;
      if (off > 1.0f) off = 1.0f;
      stops[i].color = to_color(s.argb);
      stops[i].opa   = fold_opa(c, s.argb);
      stops[i].frac  = static_cast<uint8_t>(off * 255.0f + 0.5f);
    }
    lv_vector_gradient_spread_t spread = LV_VECTOR_GRADIENT_SPREAD_PAD;
    if (grad->extend == NEUI_GRADIENT_EXTEND_REPEAT) spread = LV_VECTOR_GRADIENT_SPREAD_REPEAT;
    if (grad->extend == NEUI_GRADIENT_EXTEND_MIRROR) spread = LV_VECTOR_GRADIENT_SPREAD_REFLECT;

    if (stroke) {
      if (grad->kind == NEUI_GRADIENT_RADIAL)
        lv_draw_vector_dsc_set_stroke_radial_gradient(v, grad->start_x, grad->start_y, grad->radius);
      else
        lv_draw_vector_dsc_set_stroke_linear_gradient(v, grad->start_x, grad->start_y,
                                                      grad->end_x, grad->end_y);
      lv_draw_vector_dsc_set_stroke_gradient_spread(v, spread);
      lv_draw_vector_dsc_set_stroke_gradient_color_stops(v, stops.data(),
                                                         static_cast<uint16_t>(stops.size()));
    } else {
      if (grad->kind == NEUI_GRADIENT_RADIAL)
        lv_draw_vector_dsc_set_fill_radial_gradient(v, grad->start_x, grad->start_y, grad->radius);
      else
        lv_draw_vector_dsc_set_fill_linear_gradient(v, grad->start_x, grad->start_y,
                                                    grad->end_x, grad->end_y);
      lv_draw_vector_dsc_set_fill_gradient_spread(v, spread);
      lv_draw_vector_dsc_set_fill_gradient_color_stops(v, stops.data(),
                                                       static_cast<uint16_t>(stops.size()));
    }
  }

  static void NEUI_ABI lb_fill_path(neui_render_ctx_t raw, uint32_t argb)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->layer || !c->path) return;
    lv_draw_vector_dsc_t* v = vector_batch(c);
    if (!v) return;
    lv_matrix_t m = vector_matrix(c);
    lv_draw_vector_dsc_set_transform(v, &m);
    lv_draw_vector_dsc_set_fill_color(v, to_color(argb));
    lv_draw_vector_dsc_set_fill_opa(v, fold_opa(c, argb));
    lv_draw_vector_dsc_set_fill_rule(v, c->fill_rule == NEUI_FILL_RULE_EVENODD
                                        ? LV_VECTOR_FILL_EVENODD
                                        : LV_VECTOR_FILL_NONZERO);
    lv_draw_vector_dsc_set_stroke_opa(v, LV_OPA_TRANSP);
    lv_draw_vector_dsc_add_path(v, c->path);
  }

  static void stroke_path_impl(LvglCtx* c, float stroke_width, uint32_t argb,
                               const neui_gradient_t* grad,
                               const neui_stroke_style_t* style)
  {
    if (!c || !c->layer || !c->path || stroke_width <= 0.0f) return;
    lv_draw_vector_dsc_t* v = vector_batch(c);
    if (!v) return;
    lv_matrix_t m = vector_matrix(c);
    lv_draw_vector_dsc_set_transform(v, &m);
    lv_draw_vector_dsc_set_fill_opa(v, LV_OPA_TRANSP);
    lv_draw_vector_dsc_set_stroke_width(v, stroke_width);
    if (grad) {
      lv_draw_vector_dsc_set_stroke_opa(v, LV_OPA_COVER);
      set_vector_gradient(v, grad, c, /*stroke=*/true);
    } else {
      lv_draw_vector_dsc_set_stroke_color(v, to_color(argb));
      lv_draw_vector_dsc_set_stroke_opa(v, fold_opa(c, argb));
    }
    reset_stroke_style(v);
    apply_stroke_style(v, style);
    lv_draw_vector_dsc_add_path(v, c->path);
  }

  static void NEUI_ABI lb_stroke_path(neui_render_ctx_t raw, float stroke_width,
                                      uint32_t argb)
  {
    stroke_path_impl(static_cast<LvglCtx*>(raw), stroke_width, argb,
                     nullptr, nullptr);
  }

  static void NEUI_ABI lb_stroke_path_styled(neui_render_ctx_t raw,
                                             float stroke_width, uint32_t argb,
                                             const neui_stroke_style_t* style)
  {
    stroke_path_impl(static_cast<LvglCtx*>(raw), stroke_width, argb,
                     nullptr, style);
  }

  static void NEUI_ABI lb_stroke_path_gradient(neui_render_ctx_t raw,
                                               float stroke_width,
                                               const neui_gradient_t* grad,
                                               const neui_stroke_style_t* style)
  {
    if (!grad || grad->stop_count < 2 || !grad->stops) return;
    stroke_path_impl(static_cast<LvglCtx*>(raw), stroke_width, 0,
                     grad, style);
  }

  static void NEUI_ABI lb_fill_path_gradient(neui_render_ctx_t raw,
                                             const neui_gradient_t* grad)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->layer || !c->path) return;
    if (!grad || grad->stop_count < 2 || !grad->stops) return;
    lv_draw_vector_dsc_t* v = vector_batch(c);
    if (!v) return;
    lv_matrix_t m = vector_matrix(c);
    lv_draw_vector_dsc_set_transform(v, &m);
    lv_draw_vector_dsc_set_fill_opa(v, LV_OPA_COVER);
    set_vector_gradient(v, grad, c, /*stroke=*/false);
    lv_draw_vector_dsc_set_fill_rule(v, c->fill_rule == NEUI_FILL_RULE_EVENODD
                                        ? LV_VECTOR_FILL_EVENODD
                                        : LV_VECTOR_FILL_NONZERO);
    lv_draw_vector_dsc_set_stroke_opa(v, LV_OPA_TRANSP);
    lv_draw_vector_dsc_add_path(v, c->path);
  }

  static void NEUI_ABI lb_fill_rect_gradient(neui_render_ctx_t raw,
                                             float x, float y, float w, float h,
                                             const neui_gradient_t* grad)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || !c->layer || w <= 0.0f || h <= 0.0f) return;
    if (!grad || grad->stop_count < 2 || !grad->stops) return;
    lv_draw_vector_dsc_t* v = vector_batch(c);
    if (!v) return;
    lv_vector_path_t* p = lv_vector_path_create(LV_VECTOR_PATH_QUALITY_MEDIUM);
    lv_vector_path_append_rectangle(p, x, y, w, h, 0.0f, 0.0f);
    lv_matrix_t m = vector_matrix(c);
    lv_draw_vector_dsc_set_transform(v, &m);
    lv_draw_vector_dsc_set_fill_opa(v, LV_OPA_COVER);
    set_vector_gradient(v, grad, c, /*stroke=*/false);
    lv_draw_vector_dsc_set_fill_rule(v, LV_VECTOR_FILL_NONZERO);
    lv_draw_vector_dsc_set_stroke_opa(v, LV_OPA_TRANSP);
    lv_draw_vector_dsc_add_path(v, p);
    lv_vector_path_delete(p);
  }

  // -------------------------------------------------------------------------
  // Transform / alpha / font stacks

  static void NEUI_ABI lb_push_transform(neui_render_ctx_t raw)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (c) c->tf_stack.push_back(c->tf);
  }

  static void NEUI_ABI lb_pop_transform(neui_render_ctx_t raw)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c || c->tf_stack.empty()) return;
    c->tf = c->tf_stack.back();
    c->tf_stack.pop_back();
  }

  static void NEUI_ABI lb_translate(neui_render_ctx_t raw, float dx, float dy)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c) return;
    Mat23 m; m.tx = dx; m.ty = dy;
    c->tf = mat_mul(c->tf, m);
  }

  static void NEUI_ABI lb_rotate(neui_render_ctx_t raw, float radians)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c) return;
    Mat23 m;
    m.a = std::cos(radians); m.b = std::sin(radians);
    m.c = -m.b;              m.d = m.a;
    c->tf = mat_mul(c->tf, m);
  }

  static void NEUI_ABI lb_scale(neui_render_ctx_t raw, float sx, float sy)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c) return;
    Mat23 m; m.a = sx; m.d = sy;
    c->tf = mat_mul(c->tf, m);
  }

  static uint32_t NEUI_ABI lb_get_context_generation(neui_render_ctx_t)
  {
    return 0u;  // software rendering - no device loss
  }

  static void NEUI_ABI lb_push_alpha(neui_render_ctx_t raw, float factor)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c) return;
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    c->alpha_stack.push_back(factor);
  }

  static void NEUI_ABI lb_pop_alpha(neui_render_ctx_t raw)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (c && !c->alpha_stack.empty()) c->alpha_stack.pop_back();
  }

  static void NEUI_ABI lb_push_font(neui_render_ctx_t raw,
                                    const char* family_utf8, int weight)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (!c) return;
    LvglCtx::FontSel sel;
    sel.family = family_utf8 ? family_utf8 : "";
    sel.weight = weight;
    c->font_stack.push_back(std::move(sel));
  }

  static void NEUI_ABI lb_pop_font(neui_render_ctx_t raw)
  {
    auto* c = static_cast<LvglCtx*>(raw);
    if (c && !c->font_stack.empty()) c->font_stack.pop_back();
  }

  // -------------------------------------------------------------------------
  // Deferred for the prototype: off-screen surfaces + font registration.

  static neui_render_ctx_t NEUI_ABI lb_create_offscreen_context(uint32_t, uint32_t, float)
  {
    return nullptr;  // SURFACE assets degrade to asset_none (null-backend precedent)
  }

  static bool NEUI_ABI lb_read_pixels_bgra(neui_render_ctx_t, uint8_t*)
  {
    return false;
  }

  static bool NEUI_ABI lb_register_font(const uint8_t*, uint32_t,
                                        char* out_family, uint32_t cap, uint64_t* out_token)
  {
    if (out_family && cap) out_family[0] = '\0';
    if (out_token) *out_token = 0;
    return false;
  }

  static bool NEUI_ABI lb_register_font_file(const char*, char* out_family,
                                             uint32_t cap, uint64_t* out_token)
  {
    if (out_family && cap) out_family[0] = '\0';
    if (out_token) *out_token = 0;
    return false;
  }

  static void NEUI_ABI lb_unregister_font(uint64_t) {}

  // -------------------------------------------------------------------------

  static neui_render_backend_t backend = {
    NEUI_VERSION,
    lb_create_context,
    lb_destroy_context,
    lb_resize,
    lb_begin_frame,
    lb_end_frame,
    lb_fill_rect,
    lb_draw_rect,
    lb_get_scale_factor,
    lb_update_dpi,
    lb_draw_text,
    lb_measure_text,
    lb_push_clip,
    lb_pop_clip,
    lb_create_bitmap,
    lb_destroy_bitmap,
    lb_draw_bitmap,
    lb_begin_path,
    lb_move_to,
    lb_line_to,
    lb_arc,
    lb_close_path,
    lb_fill_path,
    lb_stroke_path,
    lb_push_transform,
    lb_pop_transform,
    lb_translate,
    lb_rotate,
    lb_scale,
    lb_get_context_generation,
    lb_push_alpha,
    lb_pop_alpha,
    lb_push_font,
    lb_pop_font,
    lb_create_offscreen_context,
    lb_read_pixels_bgra,
    lb_register_font,
    lb_register_font_file,
    lb_unregister_font,
    lb_fill_rect_gradient,
    lb_fill_path_gradient,
    lb_cubic_to,
    lb_quad_to,
    lb_set_fill_rule,
    lb_stroke_path_styled,
    lb_stroke_path_gradient,
  };

  neui_render_backend_t* get_backend() { return &backend; }

} // namespace neui_lvgl_backend
