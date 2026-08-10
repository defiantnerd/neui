// Cairo software rendering backend for the crossplatform host on Linux.
//
// Shape mirror of backends/cg/cg_backend.mm, adapted to Cairo's immediate-mode
// 2D API. Key differences from CG:
//   - Cairo is top-left, Y-down natively: no Y-flip anywhere (CG counter-flips
//     for text / bitmaps / arcs because AppKit's isFlipped CTM mirrors Y).
//   - The cairo_t lives for the whole context lifetime (created in
//     create_context, not rebound per frame), so there is no set_current_frame.
//   - end_frame blits the in-memory image surface to the X11 window via
//     XShmPutImage (fallback XPutImage). The backend holds the per-window
//     Display*/Window so the blit targets exactly that connection - correct
//     for both standalone and embedded (DAW) windows.
//   - Text resolves through Fontconfig + cairo's FreeType font backend.
//
// All coordinates are logical px at 96 DPI; colours are 0xAARRGGBB; bitmaps
// are BGRA8 premultiplied, top-down (cairo ARGB32 on a little-endian host is
// byte order B,G,R,A = exactly that layout).

#include <cairo/cairo.h>
#include <cairo/cairo-ft.h>
#include <fontconfig/fontconfig.h>
#include <ft2build.h>
#include FT_FREETYPE_H
#include FT_TRUETYPE_TABLES_H

#include <X11/Xlib.h>
#include <X11/Xutil.h>
#include <X11/extensions/XShm.h>
#include <sys/ipc.h>
#include <sys/shm.h>

#include <cctype>
#include <cmath>
#include <cstdlib>
#include <cstring>
#include <new>
#include <string>
#include <unordered_map>
#include <vector>

#include "cairo_backend.h"
#include "../shared/backend_util.h"

namespace neui_cairo_backend
{
  // Active font state - mirror of the cg/d2d backends' FontState.
  struct FontState
  {
    std::string family;     // empty = system default sans
    int         weight = 0; // CSS 100..900; 0 = Regular (400)
  };

  // Per-context render state. One cairo_t owned for the ctx's lifetime.
  struct CairoCtx
  {
    cairo_surface_t* surface = nullptr;  // ARGB32 image surface (the backbuffer)
    cairo_t*         cr      = nullptr;   // owned; lives for the ctx, not per-frame

    uint32_t w_px = 0, h_px = 0;          // physical pixel dimensions
    float    scale = 1.0f;                // physical px per logical px (dpi/96)
    uint32_t dpi  = 96;

    std::vector<float>     alpha_stack;   // back() = cumulative opacity; empty = 1.0
    std::vector<FontState> font_stack;    // back() = active (family, weight)

    // X11 blit state (window ctxs only; all null/0 on offscreen ctxs).
    Display*        dpy    = nullptr;     // borrowed (platform owns the connection)
    Window          win    = 0;           // borrowed
    Visual*         visual = nullptr;
    int             depth  = 0;
    GC              gc     = nullptr;      // owned
    XImage*         ximage = nullptr;      // owned; wraps `surface` data for the blit
    XShmSegmentInfo shm{};
    bool            use_shm = false;

    bool is_offscreen = false;            // backs NEUI_ASSET_KIND_SURFACE
    bool eo_fill      = false;            // even-odd fill rule (reset on begin_path)
  };

  // --------------------------------------------------------------------------
  // Helpers

  static inline void argb_to_rgba(uint32_t argb, double out[4], float alpha_mul = 1.0f)
  {
    neui_detail::argb_unpack(argb, out, alpha_mul);
  }

  static inline float current_alpha(const CairoCtx* st)
  {
    return neui_detail::alpha_stack_current(st->alpha_stack);
  }

  // --------------------------------------------------------------------------
  // Font resolution: Fontconfig match -> cairo FreeType font face, cached
  // process-wide and never evicted (the set of distinct family+weight tuples
  // in a typical app is tiny; mirrors the cg backend's CTFont cache). The
  // face is size-independent; the per-call font size is applied via
  // cairo_set_font_size, and the ctx CTM (baseline scale) handles HiDPI, so
  // measure_text always returns logical-pixel advances.

  // CSS weight (100..900, 0 = unset) -> Fontconfig FC_WEIGHT_* bucket.
  static int css_weight_to_fc(int weight)
  {
    if (weight <= 0)  return FC_WEIGHT_REGULAR;
    if (weight < 150) return FC_WEIGHT_THIN;        // 100
    if (weight < 250) return FC_WEIGHT_EXTRALIGHT;  // 200
    if (weight < 350) return FC_WEIGHT_LIGHT;       // 300
    if (weight < 450) return FC_WEIGHT_REGULAR;     // 400
    if (weight < 550) return FC_WEIGHT_MEDIUM;      // 500
    if (weight < 650) return FC_WEIGHT_SEMIBOLD;    // 600
    if (weight < 750) return FC_WEIGHT_BOLD;        // 700
    if (weight < 850) return FC_WEIGHT_EXTRABOLD;   // 800
    return FC_WEIGHT_BLACK;                          // 900
  }

  static std::unordered_map<std::string, cairo_font_face_t*>& face_cache()
  {
    static std::unordered_map<std::string, cairo_font_face_t*> cache;
    return cache;
  }

  // --- Client-registered fonts (NEUI_ASSET_KIND_FONT) -----------------------
  //
  // App fonts resolve via FreeType directly (not Fontconfig, which has no
  // memory-add API), so they are looked up by family before the Fontconfig
  // path and are NOT entered into face_cache() - that keeps the never-evicted
  // cache free of faces that a best-effort unregister destroys. Each entry is
  // one file = one weight; a multi-weight family is several entries sharing a
  // family name, and the active weight picks the nearest weight_class.
  struct AppFontEntry {
    uint64_t           token        = 0;
    FT_Face            ft_face      = nullptr;  // memory form references the asset-store bytes
    cairo_font_face_t* cr_face      = nullptr;
    int                weight_class = 400;       // OS/2 usWeightClass (100..900)
    std::string        family;
    bool               is_file      = false;
  };
  static std::vector<AppFontEntry>& app_fonts()
  {
    static std::vector<AppFontEntry> v;
    return v;
  }
  static uint64_t g_next_font_token = 1;

  static FT_Library g_ft_lib = nullptr;
  static bool ensure_ft()
  {
    if (g_ft_lib) return true;
    return FT_Init_FreeType(&g_ft_lib) == 0;
  }

  static bool iequals(const std::string& a, const std::string& b)
  {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i) {
      unsigned char ca = static_cast<unsigned char>(a[i]);
      unsigned char cb = static_cast<unsigned char>(b[i]);
      if (std::tolower(ca) != std::tolower(cb)) return false;
    }
    return true;
  }

  // The OS/2 weight class of an FT face, 400/700 fallback from the style bit.
  static int ft_weight_class(FT_Face face)
  {
    if (!face) return 400;
    TT_OS2* os2 = static_cast<TT_OS2*>(FT_Get_Sfnt_Table(face, FT_SFNT_OS2));
    if (os2 && os2->usWeightClass) return static_cast<int>(os2->usWeightClass);
    return (face->style_flags & FT_STYLE_FLAG_BOLD) ? 700 : 400;
  }

  // Pick the registered app face whose family matches `family` and whose
  // weight is nearest the requested weight (0 => 400). Null if none match.
  static cairo_font_face_t* find_app_face(const std::string& family, int weight)
  {
    if (family.empty()) return nullptr;
    const int want = weight > 0 ? weight : 400;
    cairo_font_face_t* best = nullptr;
    int best_diff = 1 << 30;
    for (auto& e : app_fonts()) {
      if (!e.cr_face || !iequals(e.family, family)) continue;
      int diff = e.weight_class - want;
      if (diff < 0) diff = -diff;
      if (diff < best_diff) { best_diff = diff; best = e.cr_face; }
    }
    return best;
  }

  // Resolve the active (family, weight) from the ctx font stack into a cached
  // cairo_font_face_t. Empty family => Fontconfig's default sans. Unknown
  // families degrade gracefully (Fontconfig always substitutes a real font).
  static cairo_font_face_t* get_active_face(const CairoCtx* st)
  {
    const FontState* fs = (st && !st->font_stack.empty())
      ? &st->font_stack.back() : nullptr;
    const std::string family = fs ? fs->family : std::string();
    const int         weight = fs ? fs->weight : 0;

    // Client-registered fonts win over Fontconfig (and bypass face_cache).
    if (cairo_font_face_t* app = find_app_face(family, weight)) return app;

    std::string key = family;
    key += '|';
    key += std::to_string(weight);

    auto& cache = face_cache();
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    static bool fc_ready = false;
    if (!fc_ready) { FcInit(); fc_ready = true; }

    FcPattern* pat = FcPatternCreate();
    if (!family.empty())
      FcPatternAddString(pat, FC_FAMILY,
                         reinterpret_cast<const FcChar8*>(family.c_str()));
    FcPatternAddInteger(pat, FC_WEIGHT, css_weight_to_fc(weight));
    FcConfigSubstitute(nullptr, pat, FcMatchPattern);
    FcDefaultSubstitute(pat);

    FcResult   res;
    FcPattern* matched = FcFontMatch(nullptr, pat, &res);
    FcPatternDestroy(pat);
    if (!matched) { cache[key] = nullptr; return nullptr; }

    // cairo references the pattern; we keep `matched` alive for the process
    // (the cache never evicts) so the face stays valid - mirrors the cg
    // backend's never-released CTFont cache.
    cairo_font_face_t* face = cairo_ft_font_face_create_for_pattern(matched);
    cache[key] = face;
    return face;
  }

  // Set the cr's font face + size for the active font state. Returns false
  // when there is no usable face. font_size is logical px.
  static bool apply_font(CairoCtx* st, float font_size)
  {
    if (font_size <= 0.0f) return false;
    cairo_font_face_t* face = get_active_face(st);
    if (!face) return false;
    cairo_set_font_face(st->cr, face);
    cairo_set_font_size(st->cr, font_size);
    return true;
  }

  // --------------------------------------------------------------------------
  // X11 backbuffer management (window ctxs only).

  // Async SHM-attach error detection: XShmAttach reports failure via a
  // BadAccess X error delivered later. Install a scoped handler around the
  // attach+sync so we can fall back to plain XImage cleanly.
  static bool   g_shm_attach_failed = false;
  static int (*g_prev_x_error)(Display*, XErrorEvent*) = nullptr;
  static int shm_attach_error_handler(Display*, XErrorEvent*)
  {
    g_shm_attach_failed = true;
    return 0;
  }

  static int cairo_stride(uint32_t w_px)
  {
    return cairo_format_stride_for_width(CAIRO_FORMAT_ARGB32,
                                         static_cast<int>(w_px));
  }

  static void free_window_surface(CairoCtx* st)
  {
    if (st->cr)      { cairo_destroy(st->cr);            st->cr = nullptr; }
    if (st->surface) { cairo_surface_destroy(st->surface); st->surface = nullptr; }
    if (st->ximage) {
      // XDestroyImage frees ximage->data; for the non-SHM path that data
      // pointer aliases the cairo surface buffer (already freed above), and
      // for the SHM path it aliases shm.shmaddr (freed below). Detach the
      // data pointer first so XDestroyImage only frees the XImage struct.
      st->ximage->data = nullptr;
      XDestroyImage(st->ximage);
      st->ximage = nullptr;
    }
    if (st->use_shm && st->dpy && st->shm.shmaddr) {
      XShmDetach(st->dpy, &st->shm);
      shmdt(st->shm.shmaddr);
      st->shm.shmaddr = nullptr;
      st->use_shm = false;
    }
  }

  // Allocate the cairo surface + cairo_t + XImage at the ctx's current
  // w_px/h_px. SHM-first with a malloc-XImage fallback.
  static void alloc_window_surface(CairoCtx* st)
  {
    const int stride = cairo_stride(st->w_px);

    // --- Try MIT-SHM (zero-copy: cairo renders straight into the shm buffer).
    if (st->dpy && XShmQueryExtension(st->dpy)) {
      std::memset(&st->shm, 0, sizeof(st->shm));
      st->ximage = XShmCreateImage(st->dpy, st->visual, static_cast<unsigned int>(st->depth), ZPixmap,
                                   nullptr, &st->shm, st->w_px, st->h_px);
      if (st->ximage) {
        // Honour cairo's stride for the shm allocation so the surface and
        // the XImage agree on bytes-per-line.
        st->ximage->bytes_per_line = stride;
        size_t bytes = static_cast<size_t>(stride) * st->h_px;
        st->shm.shmid = shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
        if (st->shm.shmid != -1) {
          st->shm.shmaddr = static_cast<char*>(shmat(st->shm.shmid, nullptr, 0));
          if (st->shm.shmaddr != reinterpret_cast<char*>(-1)) {
            st->ximage->data    = st->shm.shmaddr;
            st->shm.readOnly    = False;

            g_shm_attach_failed = false;
            g_prev_x_error = XSetErrorHandler(shm_attach_error_handler);
            Bool attached = XShmAttach(st->dpy, &st->shm);
            XSync(st->dpy, False);
            XSetErrorHandler(g_prev_x_error);

            // Mark the segment for deletion now; the kernel reclaims it once
            // we detach (or on process exit), so we never leak shm ids.
            shmctl(st->shm.shmid, IPC_RMID, nullptr);

            if (attached && !g_shm_attach_failed) {
              st->surface = cairo_image_surface_create_for_data(
                reinterpret_cast<unsigned char*>(st->shm.shmaddr),
                CAIRO_FORMAT_ARGB32, static_cast<int>(st->w_px), static_cast<int>(st->h_px), stride);
              if (cairo_surface_status(st->surface) == CAIRO_STATUS_SUCCESS) {
                st->use_shm = true;
                st->cr = cairo_create(st->surface);
                return;
              }
              // surface creation failed - tear down shm + fall through.
              cairo_surface_destroy(st->surface);
              st->surface = nullptr;
            }
            // attach failed - detach + free, fall through to malloc path.
            XShmDetach(st->dpy, &st->shm);
            shmdt(st->shm.shmaddr);
            st->shm.shmaddr = nullptr;
          } else {
            st->shm.shmaddr = nullptr;
          }
        }
        // shm path failed past XShmCreateImage - drop the image and retry.
        if (st->ximage) {
          st->ximage->data = nullptr;
          XDestroyImage(st->ximage);
          st->ximage = nullptr;
        }
      }
    }

    // --- Fallback: cairo owns the buffer; wrap it in a plain XImage for
    //     XPutImage. (Remote display, denied SHM, or no MIT-SHM.)
    st->use_shm = false;
    st->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32,
                                             static_cast<int>(st->w_px), static_cast<int>(st->h_px));
    st->cr = cairo_create(st->surface);
    if (st->dpy) {
      unsigned char* data = cairo_image_surface_get_data(st->surface);
      st->ximage = XCreateImage(st->dpy, st->visual, static_cast<unsigned int>(st->depth), ZPixmap, 0,
                                reinterpret_cast<char*>(data),
                                st->w_px, st->h_px, 32, stride);
    }
  }

  // --------------------------------------------------------------------------
  // Context lifecycle

  static neui_render_ctx_t cairo_create_context(void* native_handle,
                                                uint32_t width, uint32_t height)
  {
    auto* st = new CairoCtx();
    if (native_handle) {
      auto* ns = static_cast<LinuxNativeSurface*>(native_handle);
      st->dpy    = ns->dpy;
      st->win    = ns->win;
      st->visual = ns->visual;
      st->depth  = ns->depth;
    }
    st->w_px = width  ? width  : 1;
    st->h_px = height ? height : 1;
    if (st->dpy) {
      st->gc = XCreateGC(st->dpy, st->win, 0, nullptr);
      alloc_window_surface(st);
    }
    return st;
  }

  static void cairo_destroy_context(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st) return;
    if (st->is_offscreen) {
      if (st->cr)      cairo_destroy(st->cr);
      if (st->surface) cairo_surface_destroy(st->surface);
    } else {
      free_window_surface(st);
      if (st->gc && st->dpy) XFreeGC(st->dpy, st->gc);
    }
    delete st;
  }

  static void cairo_resize(neui_render_ctx_t raw, uint32_t width, uint32_t height)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || st->is_offscreen) return;   // offscreen ctxs are fixed-size
    if (width == 0)  width  = 1;
    if (height == 0) height = 1;
    if (width == st->w_px && height == st->h_px) return;
    free_window_surface(st);
    st->w_px = width;
    st->h_px = height;
    alloc_window_surface(st);
  }

  static void cairo_begin_frame(neui_render_ctx_t raw, uint32_t clear_argb)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    // Outer save - paired with end_frame's restore so an unbalanced
    // push_clip / push_transform inside the frame can't leak across frames.
    cairo_save(st->cr);
    st->alpha_stack.clear();
    st->font_stack.clear();

    // Baseline scale: a logical-px draw maps to (scale) device px. Applied
    // here (not at create) so update_dpi takes effect on the next frame.
    if (st->scale != 1.0f)
      cairo_scale(st->cr, st->scale, st->scale);

    // Clear the whole surface. cairo_paint fills the entire clip region with
    // the source regardless of the CTM (a solid colour pattern is
    // CTM-independent), so this covers all physical pixels.
    double rgba[4]; argb_to_rgba(clear_argb, rgba);
    cairo_set_source_rgba(st->cr, rgba[0], rgba[1], rgba[2], rgba[3]);
    cairo_set_operator(st->cr, CAIRO_OPERATOR_SOURCE);
    cairo_paint(st->cr);
    cairo_set_operator(st->cr, CAIRO_OPERATOR_OVER);
  }

  static void cairo_end_frame(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    cairo_restore(st->cr);
    cairo_surface_flush(st->surface);

    // Offscreen ctxs have nothing to present.
    if (st->is_offscreen || !st->dpy || !st->ximage) return;

    if (st->use_shm) {
      // XShmPutImage is asynchronous: the server reads our shared buffer
      // during request execution, but the client doesn't know when that
      // finished. We reuse the SAME buffer for the next frame, so we must
      // not start overwriting it until the read completes - otherwise a
      // continuously-animating UI tears/flickers. XSync round-trips, which
      // guarantees the server has executed the put (incl. the shm read)
      // before we return. (A completion-event handshake would avoid the
      // round-trip, but at neui's redraw rates the sync cost is negligible.)
      XShmPutImage(st->dpy, st->win, st->gc, st->ximage,
                   0, 0, 0, 0, st->w_px, st->h_px, False);
      XSync(st->dpy, False);
    } else {
      // XPutImage copies the pixels into the protocol request, so the buffer
      // is free to reuse as soon as the request is flushed.
      XPutImage(st->dpy, st->win, st->gc, st->ximage,
                0, 0, 0, 0, st->w_px, st->h_px);
      XFlush(st->dpy);
    }
  }

  // --------------------------------------------------------------------------
  // Primitive draws

  static void cairo_fill_rect(neui_render_ctx_t raw,
                              float x, float y, float w, float h, uint32_t argb)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    double rgba[4]; argb_to_rgba(argb, rgba, current_alpha(st));
    cairo_set_source_rgba(st->cr, rgba[0], rgba[1], rgba[2], rgba[3]);
    cairo_rectangle(st->cr, x, y, w, h);
    cairo_fill(st->cr);
  }

  static void cairo_draw_rect(neui_render_ctx_t raw,
                              float x, float y, float w, float h,
                              float stroke_width, uint32_t argb)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    double rgba[4]; argb_to_rgba(argb, rgba, current_alpha(st));
    cairo_set_source_rgba(st->cr, rgba[0], rgba[1], rgba[2], rgba[3]);
    cairo_set_line_width(st->cr, stroke_width);
    // Stroke the exact rect, centered on the edge - matches d2d
    // (DrawRectangle(x,y,x+w,y+h)) and cg (CGContextStrokeRect(x,y,w,h)), so a
    // 1px border/outline lands identically across all three backends.
    cairo_rectangle(st->cr, x, y, w, h);
    cairo_stroke(st->cr);
  }

  static float cairo_get_scale_factor(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    return st ? st->scale : 1.0f;
  }

  static void cairo_update_dpi(neui_render_ctx_t raw, uint32_t dpi)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st) return;
    st->dpi   = dpi;
    st->scale = static_cast<float>(dpi) / 96.0f;
  }

  // Draw UTF-8 text into (x,y,w,h) logical px. Vertical-centered (matches the
  // d2d/cg backends), left-aligned horizontally (callers pre-shift x for
  // center/right). Clipped to the rect so over-long strings don't bleed.
  static void cairo_draw_text(neui_render_ctx_t raw,
                              float x, float y, float w, float h,
                              const char* text, float font_size, uint32_t argb)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr || !text || !*text || font_size <= 0.0f) return;

    cairo_save(st->cr);
    cairo_rectangle(st->cr, x, y, w, h);
    cairo_clip(st->cr);

    if (!apply_font(st, font_size)) { cairo_restore(st->cr); return; }

    // Split on '\n' (tolerating CRLF) so embedded newlines render as multiple
    // lines, matching the DirectWrite backend (which breaks on '\n'). The
    // common single-line case yields one entry and behaves as before. cairo's
    // show_text itself does not interpret '\n', so we lay the lines out here.
    std::vector<std::string> lines;
    for (const char* start = text, *p = text;; ++p) {
      if (*p == '\n' || *p == '\0') {
        size_t len = static_cast<size_t>(p - start);
        if (len && start[len - 1] == '\r') --len;   // strip CR of a CRLF
        lines.emplace_back(start, len);
        if (*p == '\0') break;
        start = p + 1;
      }
    }

    cairo_font_extents_t fe;
    cairo_font_extents(st->cr, &fe);
    double line_h   = fe.ascent + fe.descent;
    double block_h  = line_h * static_cast<double>(lines.size());
    // Vertically centre the whole line block within the rect (one line ==
    // the previous single-line centring).
    double text_top = y + (h - block_h) * 0.5;
    if (text_top < y) text_top = y;
    double baseline_y = text_top + fe.ascent;

    double rgba[4]; argb_to_rgba(argb, rgba, current_alpha(st));
    cairo_set_source_rgba(st->cr, rgba[0], rgba[1], rgba[2], rgba[3]);
    for (const auto& ln : lines) {
      if (!ln.empty()) {
        cairo_move_to(st->cr, x, baseline_y);
        cairo_show_text(st->cr, ln.c_str());
      }
      baseline_y += line_h;
    }

    cairo_restore(st->cr);
  }

  // Vertical metrics of the active font. line_height must match what
  // cairo_draw_text uses for block layout above: cairo_font_extents' ascent +
  // descent, deliberately WITHOUT fe.height (which folds in leading) so the
  // two stay in lockstep.
  static void cairo_font_metrics(neui_render_ctx_t raw, float font_size,
                                  float* out_ascent, float* out_descent,
                                  float* out_line_height)
  {
    if (out_ascent)      *out_ascent      = 0.0f;
    if (out_descent)     *out_descent     = 0.0f;
    if (out_line_height) *out_line_height = 0.0f;
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr || font_size <= 0.0f) return;
    if (!apply_font(st, font_size)) return;
    cairo_font_extents_t fe;
    cairo_font_extents(st->cr, &fe);
    if (out_ascent)      *out_ascent      = static_cast<float>(fe.ascent);
    if (out_descent)     *out_descent     = static_cast<float>(fe.descent);
    if (out_line_height) *out_line_height =
      static_cast<float>(fe.ascent + fe.descent);
  }

  static float cairo_measure_text(neui_render_ctx_t raw,
                                  const char* text, int text_len, float font_size)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr || !text || !*text || font_size <= 0.0f) return 0.0f;
    if (!apply_font(st, font_size)) return 0.0f;

    cairo_text_extents_t te;
    if (text_len < 0) {
      cairo_text_extents(st->cr, text, &te);
    } else {
      std::string slice(text, static_cast<size_t>(text_len));
      cairo_text_extents(st->cr, slice.c_str(), &te);
    }
    // x_advance is the pen advance (what callers want for caret math /
    // button auto-size), in user space = logical px.
    return static_cast<float>(te.x_advance);
  }

  static void cairo_push_clip(neui_render_ctx_t raw,
                              float x, float y, float w, float h)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    cairo_save(st->cr);
    cairo_rectangle(st->cr, x, y, w, h);
    cairo_clip(st->cr);
  }

  static void cairo_pop_clip(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    cairo_restore(st->cr);
  }

  // --------------------------------------------------------------------------
  // Bitmaps

  struct CairoBitmap
  {
    cairo_surface_t* surf      = nullptr;
    unsigned char*   data      = nullptr;  // owned copy
    uint32_t         width_px  = 0;
    uint32_t         height_px = 0;
    float            scale     = 1.0f;
  };

  static void* cairo_create_bitmap(neui_render_ctx_t /*raw*/,
                                   uint32_t width_px, uint32_t height_px,
                                   const uint8_t* bgra_pixels, float scale)
  {
    if (!bgra_pixels || width_px == 0 || height_px == 0) return nullptr;
    if (scale <= 0.0f) scale = 1.0f;

    const int stride = cairo_stride(width_px);
    size_t total = static_cast<size_t>(stride) * height_px;
    auto* owned = static_cast<unsigned char*>(std::malloc(total));
    if (!owned) return nullptr;
    // Copy row by row to honour cairo's (possibly padded) stride. The input
    // is tightly packed width_px*4.
    const size_t src_row = static_cast<size_t>(width_px) * 4u;
    for (uint32_t row = 0; row < height_px; ++row)
      std::memcpy(owned + static_cast<size_t>(row) * static_cast<size_t>(stride),
                  bgra_pixels + static_cast<size_t>(row) * src_row, src_row);

    cairo_surface_t* surf = cairo_image_surface_create_for_data(
      owned, CAIRO_FORMAT_ARGB32, static_cast<int>(width_px), static_cast<int>(height_px), stride);
    if (cairo_surface_status(surf) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(surf);
      std::free(owned);
      return nullptr;
    }

    auto* h = new CairoBitmap();
    h->surf = surf; h->data = owned;
    h->width_px = width_px; h->height_px = height_px; h->scale = scale;
    return h;
  }

  static void cairo_destroy_bitmap(neui_render_ctx_t /*raw*/, void* bitmap)
  {
    if (!bitmap) return;
    auto* h = static_cast<CairoBitmap*>(bitmap);
    if (h->surf) cairo_surface_destroy(h->surf);
    std::free(h->data);
    delete h;
  }

  static void cairo_draw_bitmap(neui_render_ctx_t raw, void* bitmap,
                                float src_x, float src_y, float src_w, float src_h,
                                float dst_x, float dst_y, float dst_w, float dst_h,
                                uint32_t tint)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr || !bitmap) return;
    auto* h = static_cast<CairoBitmap*>(bitmap);
    if (!h->surf || dst_w <= 0.0f || dst_h <= 0.0f) return;

    // Source region in surface (physical) pixels. src_*<=0 => whole bitmap.
    double sxp = (src_w > 0.0f) ? src_x * h->scale : 0.0;
    double syp = (src_h > 0.0f) ? src_y * h->scale : 0.0;
    double swp = (src_w > 0.0f) ? src_w * h->scale : static_cast<double>(h->width_px);
    double shp = (src_h > 0.0f) ? src_h * h->scale : static_cast<double>(h->height_px);
    if (swp <= 0.0 || shp <= 0.0) return;

    cairo_save(st->cr);
    // Map the source physical-px region onto the dst logical rect.
    cairo_translate(st->cr, dst_x, dst_y);
    cairo_scale(st->cr, dst_w / swp, dst_h / shp);
    // After the scale, 1 user unit == 1 surface px; place the surface so the
    // requested sub-region origin lands at (0,0).
    cairo_set_source_surface(st->cr, h->surf, -sxp, -syp);
    cairo_rectangle(st->cr, 0, 0, swp, shp);
    cairo_clip(st->cr);

    if (tint == 0xFFFFFFFFu) {
      cairo_paint_with_alpha(st->cr, current_alpha(st));
    } else {
      // Approximate the d2d/cg multiplicative tint: lay down the image, then
      // multiply the tint RGB over it masked by the image's own alpha. The
      // composite alpha folds in the tint alpha * the alpha stack so a
      // translucent tint lets the background bleed through.
      double rgba[4]; argb_to_rgba(tint, rgba);
      cairo_paint_with_alpha(st->cr, current_alpha(st) * rgba[3]);
      cairo_set_operator(st->cr, CAIRO_OPERATOR_MULTIPLY);
      cairo_set_source_rgb(st->cr, rgba[0], rgba[1], rgba[2]);
      cairo_mask_surface(st->cr, h->surf, -sxp, -syp);
    }
    cairo_restore(st->cr);
  }

  // --------------------------------------------------------------------------
  // Path API - operates on the cr's current path (cairo retains it across
  // save/restore; begin_path discards it).

  static void cairo_begin_path(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    cairo_new_path(st->cr);
    st->eo_fill = false;
  }

  static void cairo_move_to_fn(neui_render_ctx_t raw, float x, float y)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    cairo_move_to(st->cr, x, y);
  }

  static void cairo_line_to_fn(neui_render_ctx_t raw, float x, float y)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    cairo_line_to(st->cr, x, y);
  }

  static void cairo_arc_fn(neui_render_ctx_t raw,
                           float cx, float cy, float radius,
                           float start_rad, float end_rad)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    // Cairo is Y-down: increasing angle is clockwise, matching renderer.h's
    // "positive sweep (start<end) = clockwise on screen". A reverse sweep
    // (end<start) uses cairo_arc_negative so the geometry isn't forced the
    // long way round.
    if (end_rad < start_rad)
      cairo_arc_negative(st->cr, cx, cy, radius, start_rad, end_rad);
    else
      cairo_arc(st->cr, cx, cy, radius, start_rad, end_rad);
  }

  static void cairo_close_path_fn(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    cairo_close_path(st->cr);
  }

  static void cairo_fill_path(neui_render_ctx_t raw, uint32_t argb)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    double rgba[4]; argb_to_rgba(argb, rgba, current_alpha(st));
    cairo_set_source_rgba(st->cr, rgba[0], rgba[1], rgba[2], rgba[3]);
    cairo_set_fill_rule(st->cr, st->eo_fill ? CAIRO_FILL_RULE_EVEN_ODD
                                            : CAIRO_FILL_RULE_WINDING);
    cairo_fill_preserve(st->cr);  // keep path so a later stroke_path works
    // The fill rule is persistent context state; restore WINDING so later
    // fills (fill_rect, gradients) that don't set it don't inherit even-odd.
    if (st->eo_fill) cairo_set_fill_rule(st->cr, CAIRO_FILL_RULE_WINDING);
  }

  static void cairo_stroke_path(neui_render_ctx_t raw,
                                float stroke_width, uint32_t argb)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    double rgba[4]; argb_to_rgba(argb, rgba, current_alpha(st));
    cairo_set_source_rgba(st->cr, rgba[0], rgba[1], rgba[2], rgba[3]);
    cairo_set_line_width(st->cr, stroke_width);
    cairo_stroke_preserve(st->cr);
  }

  static void cairo_cubic_to(neui_render_ctx_t raw, float c1x, float c1y,
                             float c2x, float c2y, float x, float y)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    cairo_curve_to(st->cr, c1x, c1y, c2x, c2y, x, y);
  }

  static void cairo_quad_to(neui_render_ctx_t raw, float cx, float cy, float x, float y)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    // Cairo has no native quadratic; elevate to a cubic about the current
    // point (cairo_curve_to also starts a sub-path at (0,0) if none, matching
    // the elevation when there is no current point).
    double curx = 0.0, cury = 0.0;
    if (cairo_has_current_point(st->cr))
      cairo_get_current_point(st->cr, &curx, &cury);
    double c1[2], c2[2];
    neui_detail::quad_to_cubic<double>(curx, cury, cx, cy, x, y, c1, c2);
    cairo_curve_to(st->cr, c1[0], c1[1], c2[0], c2[1], x, y);
  }

  static void cairo_set_fill_rule_fn(neui_render_ctx_t raw, neui_fill_rule_t rule)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (st) st->eo_fill = (rule == NEUI_FILL_RULE_EVENODD);
  }

  static void cairo_stroke_path_styled(neui_render_ctx_t raw, float stroke_width,
                                       uint32_t argb, const neui_stroke_style_t* style)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    if (!style) { cairo_stroke_path(raw, stroke_width, argb); return; }
    double rgba[4]; argb_to_rgba(argb, rgba, current_alpha(st));
    cairo_save(st->cr);   // line cap/join/miter/dash are cr state
    cairo_set_source_rgba(st->cr, rgba[0], rgba[1], rgba[2], rgba[3]);
    cairo_set_line_width(st->cr, stroke_width);
    cairo_set_line_cap(st->cr,
      style->cap == NEUI_LINE_CAP_ROUND  ? CAIRO_LINE_CAP_ROUND  :
      style->cap == NEUI_LINE_CAP_SQUARE ? CAIRO_LINE_CAP_SQUARE : CAIRO_LINE_CAP_BUTT);
    cairo_set_line_join(st->cr,
      style->join == NEUI_LINE_JOIN_ROUND ? CAIRO_LINE_JOIN_ROUND :
      style->join == NEUI_LINE_JOIN_BEVEL ? CAIRO_LINE_JOIN_BEVEL : CAIRO_LINE_JOIN_MITER);
    cairo_set_miter_limit(st->cr, style->miter_limit > 0.0f ? style->miter_limit : 4.0);
    if (style->dash_array && style->dash_count > 0) {
      std::vector<double> dashes(style->dash_count);
      for (uint32_t i = 0; i < style->dash_count; ++i) dashes[i] = style->dash_array[i];
      cairo_set_dash(st->cr, dashes.data(), static_cast<int>(dashes.size()), style->dash_offset);
    }
    cairo_stroke_preserve(st->cr);
    cairo_restore(st->cr);
  }

  // --------------------------------------------------------------------------
  // Gradient fills

  // Build a one-shot cairo gradient pattern from `g`, alpha-stack folded into
  // each stop. Caller owns the returned pattern (cairo_pattern_destroy).
  // Returns null on a malformed gradient. Cairo honours all three extend
  // modes natively (PAD / REPEAT / REFLECT).
  static cairo_pattern_t* cairo_make_gradient(CairoCtx* st,
                                              const neui_gradient_t* g)
  {
    if (!st || !g || !g->stops || g->stop_count < 2) return nullptr;

    cairo_pattern_t* pat =
      (g->kind == NEUI_GRADIENT_RADIAL)
        ? cairo_pattern_create_radial(g->end_x, g->end_y, 0.0,
                                       g->start_x, g->start_y, g->radius)
        : cairo_pattern_create_linear(g->start_x, g->start_y,
                                       g->end_x,   g->end_y);
    if (!pat || cairo_pattern_status(pat) != CAIRO_STATUS_SUCCESS) {
      if (pat) cairo_pattern_destroy(pat);
      return nullptr;
    }

    const float alpha = current_alpha(st);
    for (uint32_t i = 0; i < g->stop_count; ++i) {
      double rgba[4]; argb_to_rgba(g->stops[i].argb, rgba, alpha);
      double off = g->stops[i].offset;
      if (off < 0.0) off = 0.0; else if (off > 1.0) off = 1.0;
      cairo_pattern_add_color_stop_rgba(pat, off,
                                        rgba[0], rgba[1], rgba[2], rgba[3]);
    }

    cairo_extend_t ext = CAIRO_EXTEND_PAD;            // CLAMP
    if (g->extend == NEUI_GRADIENT_EXTEND_REPEAT)      ext = CAIRO_EXTEND_REPEAT;
    else if (g->extend == NEUI_GRADIENT_EXTEND_MIRROR) ext = CAIRO_EXTEND_REFLECT;
    cairo_pattern_set_extend(pat, ext);
    return pat;
  }

  static void cairo_fill_rect_gradient(neui_render_ctx_t raw,
                                       float x, float y, float w, float h,
                                       const neui_gradient_t* g)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    cairo_pattern_t* pat = cairo_make_gradient(st, g);
    if (!pat) return;
    cairo_set_source(st->cr, pat);
    cairo_rectangle(st->cr, x, y, w, h);
    cairo_fill(st->cr);
    cairo_pattern_destroy(pat);
  }

  static void cairo_fill_path_gradient(neui_render_ctx_t raw,
                                       const neui_gradient_t* g)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    cairo_pattern_t* pat = cairo_make_gradient(st, g);
    if (!pat) return;
    cairo_set_source(st->cr, pat);
    cairo_set_fill_rule(st->cr, st->eo_fill ? CAIRO_FILL_RULE_EVEN_ODD
                                            : CAIRO_FILL_RULE_WINDING);
    cairo_fill_preserve(st->cr);  // keep path so a later stroke_path works
    if (st->eo_fill) cairo_set_fill_rule(st->cr, CAIRO_FILL_RULE_WINDING);
    cairo_pattern_destroy(pat);
  }

  static void cairo_stroke_path_gradient(neui_render_ctx_t raw, float stroke_width,
                                         const neui_gradient_t* g,
                                         const neui_stroke_style_t* style)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->cr) return;
    cairo_pattern_t* pat = cairo_make_gradient(st, g);
    if (!pat) return;
    cairo_save(st->cr);   // source + line cap/join/miter/dash are cr state
    cairo_set_source(st->cr, pat);
    cairo_set_line_width(st->cr, stroke_width);
    if (style) {
      cairo_set_line_cap(st->cr,
        style->cap == NEUI_LINE_CAP_ROUND  ? CAIRO_LINE_CAP_ROUND  :
        style->cap == NEUI_LINE_CAP_SQUARE ? CAIRO_LINE_CAP_SQUARE : CAIRO_LINE_CAP_BUTT);
      cairo_set_line_join(st->cr,
        style->join == NEUI_LINE_JOIN_ROUND ? CAIRO_LINE_JOIN_ROUND :
        style->join == NEUI_LINE_JOIN_BEVEL ? CAIRO_LINE_JOIN_BEVEL : CAIRO_LINE_JOIN_MITER);
      cairo_set_miter_limit(st->cr, style->miter_limit > 0.0f ? style->miter_limit : 4.0);
      if (style->dash_array && style->dash_count > 0) {
        std::vector<double> dashes(style->dash_count);
        for (uint32_t i = 0; i < style->dash_count; ++i) dashes[i] = style->dash_array[i];
        cairo_set_dash(st->cr, dashes.data(), static_cast<int>(dashes.size()), style->dash_offset);
      }
    }
    cairo_stroke_preserve(st->cr);  // keep path so a later op still works
    cairo_restore(st->cr);
    cairo_pattern_destroy(pat);
  }

  // --------------------------------------------------------------------------
  // Transform stack - cairo_save/restore covers both CTM and clip.

  static void cairo_push_transform(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (st && st->cr) cairo_save(st->cr);
  }
  static void cairo_pop_transform(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (st && st->cr) cairo_restore(st->cr);
  }
  static void cairo_translate_fn(neui_render_ctx_t raw, float dx, float dy)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (st && st->cr) cairo_translate(st->cr, dx, dy);
  }
  static void cairo_rotate_fn(neui_render_ctx_t raw, float radians)
  {
    // Y-down: positive angle rotates clockwise on screen, matching renderer.h.
    auto* st = static_cast<CairoCtx*>(raw);
    if (st && st->cr) cairo_rotate(st->cr, radians);
  }
  static void cairo_scale_fn(neui_render_ctx_t raw, float sx, float sy)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (st && st->cr) cairo_scale(st->cr, sx, sy);
  }

  // Software backend never device-loses.
  static uint32_t cairo_get_context_generation(neui_render_ctx_t) { return 0u; }

  // --------------------------------------------------------------------------
  // Alpha stack - pure software, folded into each draw's colour / paint alpha.

  static void cairo_push_alpha(neui_render_ctx_t raw, float factor)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (st) neui_detail::alpha_stack_push(st->alpha_stack, factor);
  }
  static void cairo_pop_alpha(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (st) neui_detail::alpha_stack_pop(st->alpha_stack);
  }

  // --------------------------------------------------------------------------
  // Font stack

  static void cairo_push_font(neui_render_ctx_t raw, const char* family_utf8, int weight)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st) return;
    FontState fs;
    if (family_utf8 && *family_utf8) fs.family = family_utf8;
    fs.weight = weight;
    st->font_stack.push_back(std::move(fs));
  }
  static void cairo_pop_font(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (st && !st->font_stack.empty()) st->font_stack.pop_back();
  }

  // --------------------------------------------------------------------------
  // Client-registered fonts (NEUI_ASSET_KIND_FONT).

  static void cairo_copy_family(const char* fam, char* out, uint32_t cap)
  {
    if (!out || cap == 0) return;
    out[0] = '\0';
    if (!fam) return;
    uint32_t n = static_cast<uint32_t>(std::strlen(fam));
    if (n > cap - 1) n = cap - 1;
    if (n) std::memcpy(out, fam, n);
    out[n] = '\0';
  }

  // FreeType face (+ optional owned byte copy for the in-memory form) whose
  // teardown is handed to Cairo. destroy_app_face_data runs only when Cairo
  // releases its LAST reference to the cairo_font_face_t - which includes the
  // scaled-font instances Cairo keeps in its own process-global cache, and so
  // can outlive cairo_font_face_destroy. That is exactly why unregister must
  // NOT call FT_Done_Face eagerly (the old bug): freeing the FT_Face - or the
  // bytes it lazily reads glyphs from - out from under a still-cached scaled
  // font is a use-after-free. Freeing here guarantees FT_Done_Face runs first,
  // then the bytes the face referenced.
  struct AppFaceData { FT_Face face; uint8_t* bytes; };
  static const cairo_user_data_key_t g_app_face_key = {};
  static void destroy_app_face_data(void* p)
  {
    AppFaceData* d = static_cast<AppFaceData*>(p);
    if (!d) return;
    if (d->face) FT_Done_Face(d->face);
    delete[] d->bytes;
    delete d;
  }

  // Finish registration from a built FT face: wrap it in a cairo face, hand
  // the FT face (+ owned bytes, in-memory form) to the cairo face's lifetime,
  // read its family + weight class, store it. Consumes the face + bytes on
  // failure.
  static bool cairo_register_ft_face(FT_Face face, uint8_t* owned_bytes, bool is_file,
                                     char* out_family, uint32_t cap,
                                     uint64_t* out_token)
  {
    cairo_font_face_t* cr = cairo_ft_font_face_create_for_ft_face(face, 0);
    if (!cr || cairo_font_face_status(cr) != CAIRO_STATUS_SUCCESS) {
      if (cr) cairo_font_face_destroy(cr);
      FT_Done_Face(face);
      delete[] owned_bytes;
      return false;
    }
    AppFaceData* fd = new (std::nothrow) AppFaceData{ face, owned_bytes };
    if (!fd ||
        cairo_font_face_set_user_data(cr, &g_app_face_key, fd,
                                      destroy_app_face_data) != CAIRO_STATUS_SUCCESS) {
      delete fd;
      cairo_font_face_destroy(cr);
      FT_Done_Face(face);
      delete[] owned_bytes;
      return false;
    }
    AppFontEntry e;
    e.token        = g_next_font_token++;
    e.ft_face      = face;       // owned by the cairo face now; not freed in unregister
    e.cr_face      = cr;
    e.weight_class = ft_weight_class(face);
    e.is_file      = is_file;
    if (face->family_name) e.family = face->family_name;
    cairo_copy_family(face->family_name, out_family, cap);
    if (out_token) *out_token = e.token;
    app_fonts().push_back(std::move(e));
    return true;
  }

  static bool cairo_register_font(const uint8_t* data, uint32_t len,
                                  char* out_family, uint32_t cap,
                                  uint64_t* out_token)
  {
    if (out_family && cap) out_family[0] = '\0';
    if (out_token) *out_token = 0;
    if (!data || len == 0 || !ensure_ft()) return false;
    // Own a private copy: FT_New_Memory_Face reads glyph data from this buffer
    // for the face's lifetime, which (via Cairo's scaled-font cache) can
    // outlast both unregister and the asset store's own copy - so the buffer's
    // lifetime is tied to the FT face's, freed by destroy_app_face_data.
    uint8_t* owned = new (std::nothrow) uint8_t[len];
    if (!owned) return false;
    std::memcpy(owned, data, len);
    FT_Face face = nullptr;
    if (FT_New_Memory_Face(g_ft_lib, owned, static_cast<FT_Long>(len), 0, &face) != 0
     || !face) {
      delete[] owned;
      return false;
    }
    return cairo_register_ft_face(face, owned, /*is_file*/false, out_family, cap, out_token);
  }

  static bool cairo_register_font_file(const char* path,
                                       char* out_family, uint32_t cap,
                                       uint64_t* out_token)
  {
    if (out_family && cap) out_family[0] = '\0';
    if (out_token) *out_token = 0;
    if (!path || !*path || !ensure_ft()) return false;
    FT_Face face = nullptr;
    if (FT_New_Face(g_ft_lib, path, 0, &face) != 0 || !face) return false;
    // Also make Fontconfig aware so name lookups elsewhere resolve it too.
    FcConfigAppFontAddFile(nullptr,
                           reinterpret_cast<const FcChar8*>(path));
    return cairo_register_ft_face(face, /*owned_bytes*/nullptr, /*is_file*/true,
                                  out_family, cap, out_token);
  }

  static void cairo_unregister_font(uint64_t token)
  {
    if (!token) return;
    auto& v = app_fonts();
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (it->token != token) continue;
      // Release our reference to the cairo face only. The FT face + owned bytes
      // are freed by destroy_app_face_data when Cairo drops its LAST reference,
      // which may be a cached scaled font that outlives this call - freeing the
      // FT face eagerly here would pull it out from under that scaled font (the
      // old use-after-free). Fontconfig has no per-file remove; the file entry
      // stays in the app config (best-effort, harmless - find_app_face gates).
      if (it->cr_face) cairo_font_face_destroy(it->cr_face);
      v.erase(it);
      return;
    }
  }

  // --------------------------------------------------------------------------
  // Off-screen contexts (NEUI_ASSET_KIND_SURFACE).

  static neui_render_ctx_t cairo_create_offscreen_context(uint32_t width_px,
                                                          uint32_t height_px,
                                                          float scale)
  {
    if (width_px == 0 || height_px == 0) return nullptr;
    if (scale <= 0.0f) scale = 1.0f;

    auto* st = new CairoCtx();
    st->is_offscreen = true;
    st->w_px  = width_px;
    st->h_px  = height_px;
    st->scale = scale;
    st->dpi   = static_cast<uint32_t>(scale * 96.0f + 0.5f);
    st->surface = cairo_image_surface_create(CAIRO_FORMAT_ARGB32, static_cast<int>(width_px), static_cast<int>(height_px));
    if (cairo_surface_status(st->surface) != CAIRO_STATUS_SUCCESS) {
      cairo_surface_destroy(st->surface);
      delete st;
      return nullptr;
    }
    st->cr = cairo_create(st->surface);
    return st;
  }

  static bool cairo_read_pixels_bgra(neui_render_ctx_t raw, uint8_t* out_bgra)
  {
    auto* st = static_cast<CairoCtx*>(raw);
    if (!st || !st->is_offscreen || !st->surface || !out_bgra) return false;
    cairo_surface_flush(st->surface);
    const unsigned char* data = cairo_image_surface_get_data(st->surface);
    if (!data) return false;
    const int stride = cairo_image_surface_get_stride(st->surface);
    const size_t row  = static_cast<size_t>(st->w_px) * 4u;  // tight output rows
    for (uint32_t y = 0; y < st->h_px; ++y)
      std::memcpy(out_bgra + static_cast<size_t>(y) * row,
                  data + static_cast<size_t>(y) * static_cast<size_t>(stride), row);
    return true;
  }

  // --------------------------------------------------------------------------

  static neui_render_backend_t backend = {
    NEUI_VERSION,
    cairo_create_context,
    cairo_destroy_context,
    cairo_resize,
    cairo_begin_frame,
    cairo_end_frame,
    cairo_fill_rect,
    cairo_draw_rect,
    cairo_get_scale_factor,
    cairo_update_dpi,
    cairo_draw_text,
    cairo_measure_text,
    cairo_push_clip,
    cairo_pop_clip,
    cairo_create_bitmap,
    cairo_destroy_bitmap,
    cairo_draw_bitmap,
    cairo_begin_path,
    cairo_move_to_fn,
    cairo_line_to_fn,
    cairo_arc_fn,
    cairo_close_path_fn,
    cairo_fill_path,
    cairo_stroke_path,
    cairo_push_transform,
    cairo_pop_transform,
    cairo_translate_fn,
    cairo_rotate_fn,
    cairo_scale_fn,
    cairo_get_context_generation,
    cairo_push_alpha,
    cairo_pop_alpha,
    cairo_push_font,
    cairo_pop_font,
    cairo_create_offscreen_context,
    cairo_read_pixels_bgra,
    cairo_register_font,
    cairo_register_font_file,
    cairo_unregister_font,
    cairo_fill_rect_gradient,
    cairo_fill_path_gradient,
    cairo_cubic_to,
    cairo_quad_to,
    cairo_set_fill_rule_fn,
    cairo_stroke_path_styled,
    cairo_stroke_path_gradient,
    cairo_font_metrics,
  };

  neui_render_backend_t* get_backend() { return &backend; }

} // namespace neui_cairo_backend
