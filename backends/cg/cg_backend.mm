// CoreGraphics rendering backend for the crossplatform host on macOS.
//
// Shape mirror of backends/d2d/d2d_backend.cpp:
//   - One CGContext per frame (drawRect: hands one to begin_frame via
//     set_current_frame).
//   - Path API -> CGMutablePathRef built between begin_path and fill/stroke.
//   - Transform stack + clip stack -> CGContextSaveGState/RestoreGState.
//   - Bitmap -> CGImage / draw_bitmap (text + bitmaps come online in steps 4 & 9).
//   - Text -> CTLine / CTFramesetter (step 4).
//
// The NEUIView is configured with isFlipped=YES so drawRect:'s CTM already
// has Y increasing downward, matching the renderer.h convention. No explicit
// Y-flip in begin_frame is needed.

#import <AppKit/AppKit.h>
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>

#include <cmath>
#include <string>
#include <unordered_map>
#include <vector>

#include "cg_backend.h"

namespace neui_cg_backend
{
  // Active font state - mirror of the d2d backend's FontState. Pushed/popped
  // via push_font/pop_font; the top of the stack feeds draw_text/measure_text.
  // Empty family => system UI font (SF Pro); weight 0 => Regular (400).
  struct FontState
  {
    std::string family;     // empty = system UI font
    int         weight = 0; // CSS 100..900; 0 = Regular
  };

  // Per-window render context. The CGContextRef itself only lives for one
  // drawRect: call - the platform layer rebinds it via set_current_frame at
  // the top of every frame.
  struct CGContextState
  {
    CGContextRef     cg_ctx        = nullptr;  // owned by AppKit; valid only between set_current_frame and end_frame
    float            width         = 0.0f;     // logical (point) dimensions of the view
    float            height        = 0.0f;
    float            backing_scale = 1.0f;     // backing scale factor (1.0 @1x, 2.0 @2x)
    uint32_t         dpi           = 96;       // 96 * backing_scale, kept in sync via update_dpi
    CGMutablePathRef path          = nullptr;  // current path under construction
    CGPoint          cursor        = CGPointZero;

    // Alpha stack. back() = effective cumulative opacity. Empty = 1.0.
    // Multiplied into the alpha component of every draw colour. Reset on
    // every begin_frame.
    std::vector<float> alpha_stack;

    // Font stack. back() = active (family, weight) for draw_text/measure_text;
    // size stays a per-call parameter. Empty = system UI font / Regular.
    // Reset on every begin_frame (mirror of the d2d backend).
    std::vector<FontState> font_stack;
  };

  // ---------------------------------------------------------------------------
  // Public hook called from the platform layer at the top of drawRect:.

  void set_current_frame(neui_render_ctx_t handle, void* cg_context,
                         float width_logical, float height_logical)
  {
    auto* st = static_cast<CGContextState*>(handle);
    if (!st) return;
    st->cg_ctx = static_cast<CGContextRef>(cg_context);
    st->width  = width_logical;
    st->height = height_logical;
  }

  // ---------------------------------------------------------------------------
  // Helpers

  static inline CGFloat ch(uint32_t comp)
  {
    return static_cast<CGFloat>(comp) / 255.0f;
  }

  static inline void argb_to_rgba(uint32_t argb, CGFloat out[4], float alpha_mul = 1.0f)
  {
    out[0] = ch((argb >> 16) & 0xFF);  // R
    out[1] = ch((argb >>  8) & 0xFF);  // G
    out[2] = ch( argb        & 0xFF);  // B
    out[3] = ch((argb >> 24) & 0xFF) * static_cast<CGFloat>(alpha_mul);  // A
  }

  static inline float current_alpha(const CGContextState* st)
  {
    return st->alpha_stack.empty() ? 1.0f : st->alpha_stack.back();
  }

  // CTFont cache - keyed by "family|weight|size_q10". Process-wide and never
  // evicted; the number of distinct (family, weight, size) tuples used in
  // typical apps is tiny (parallels d2d_backend's IDWriteTextFormat cache).
  static std::unordered_map<std::string, CTFontRef>& font_cache()
  {
    static std::unordered_map<std::string, CTFontRef> cache;
    return cache;
  }

  // CSS-style weight (100..900, 0 = unset) -> AppKit NSFontWeight scale.
  // Mirror of the d2d backend's normalise_weight mapping.
  static CGFloat css_weight_to_nsfontweight(int weight)
  {
    if (weight <= 0)  return NSFontWeightRegular;
    if (weight < 150) return NSFontWeightUltraLight; // 100
    if (weight < 250) return NSFontWeightThin;       // 200
    if (weight < 350) return NSFontWeightLight;      // 300
    if (weight < 450) return NSFontWeightRegular;    // 400
    if (weight < 550) return NSFontWeightMedium;     // 500
    if (weight < 650) return NSFontWeightSemibold;   // 600
    if (weight < 750) return NSFontWeightBold;       // 700
    if (weight < 850) return NSFontWeightHeavy;      // 800
    return NSFontWeightBlack;                         // 900
  }

  // Resolve the active font (top of the ctx's font stack, or system default
  // when empty) at the given size into a cached CTFontRef. Empty family =>
  // system UI font at the requested weight; a named family resolves via a
  // weight-traited font descriptor, falling back to the system font when the
  // family is unavailable (e.g. a Windows family name like "Consolas").
  static CTFontRef get_active_font(const CGContextState* st, float font_size)
  {
    if (font_size <= 0.0f) return nullptr;

    const FontState* fs = (st && !st->font_stack.empty())
      ? &st->font_stack.back() : nullptr;
    const std::string family = fs ? fs->family : std::string();
    const int         weight = fs ? fs->weight : 0;

    std::string key = family;
    key += '|';
    key += std::to_string(weight);
    key += '|';
    key += std::to_string(static_cast<uint32_t>(font_size * 10.0f + 0.5f));

    auto& cache = font_cache();
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    CGFloat ns_weight = css_weight_to_nsfontweight(weight);
    NSFont* font = nil;
    if (family.empty()) {
      // SF Pro on modern macOS, at the requested weight.
      font = [NSFont systemFontOfSize:static_cast<CGFloat>(font_size)
                                weight:ns_weight];
    } else {
      NSString* fam = [NSString stringWithUTF8String:family.c_str()];
      if (fam) {
        NSFontDescriptor* desc = [NSFontDescriptor fontDescriptorWithFontAttributes:@{
          NSFontFamilyAttribute : fam,
          NSFontTraitsAttribute : @{ NSFontWeightTrait : @(ns_weight) },
        }];
        font = [NSFont fontWithDescriptor:desc size:static_cast<CGFloat>(font_size)];
      }
      if (!font)  // unknown family -> graceful fallback to the system font
        font = [NSFont systemFontOfSize:static_cast<CGFloat>(font_size)
                                  weight:ns_weight];
    }
    // NSFont is toll-free bridged with CTFont; retain a +1 ref for the cache.
    CTFontRef ctf = font ? (CTFontRef)CFBridgingRetain(font) : nullptr;
    if (ctf) cache[key] = ctf;
    return ctf;
  }

  // Build a one-line CTLine for `text_len` bytes of UTF-8 (-1 = full string)
  // at the given font size, using the ctx's active font (family + weight from
  // the font stack). Caller releases via CFRelease.
  static CTLineRef make_ctline(const CGContextState* st,
                                const char* text, int text_len, float font_size)
  {
    if (!text || font_size <= 0.0f) return nullptr;
    CTFontRef font = get_active_font(st, font_size);
    if (!font) return nullptr;

    CFStringRef cf_str;
    if (text_len < 0) {
      cf_str = CFStringCreateWithCString(kCFAllocatorDefault, text,
                                          kCFStringEncodingUTF8);
    } else {
      cf_str = CFStringCreateWithBytes(kCFAllocatorDefault,
                                        reinterpret_cast<const UInt8*>(text),
                                        static_cast<CFIndex>(text_len),
                                        kCFStringEncodingUTF8,
                                        false);
    }
    if (!cf_str) return nullptr;
    if (CFStringGetLength(cf_str) == 0) {
      CFRelease(cf_str);
      return nullptr;
    }

    CFStringRef keys[]   = { kCTFontAttributeName };
    CFTypeRef   values[] = { font };
    CFDictionaryRef attrs = CFDictionaryCreate(
      kCFAllocatorDefault,
      reinterpret_cast<const void**>(keys),
      reinterpret_cast<const void**>(values),
      1,
      &kCFTypeDictionaryKeyCallBacks,
      &kCFTypeDictionaryValueCallBacks);
    if (!attrs) { CFRelease(cf_str); return nullptr; }

    CFAttributedStringRef as = CFAttributedStringCreate(kCFAllocatorDefault,
                                                         cf_str, attrs);
    CFRelease(attrs);
    CFRelease(cf_str);
    if (!as) return nullptr;

    CTLineRef line = CTLineCreateWithAttributedString(as);
    CFRelease(as);
    return line;
  }

  // ---------------------------------------------------------------------------

  static neui_render_ctx_t cg_create_context(void* native_handle,
                                              uint32_t width, uint32_t height)
  {
    auto* st = new CGContextState();
    st->width  = static_cast<float>(width);
    st->height = static_cast<float>(height);
    if (native_handle) {
      NSView* view = (__bridge NSView*)native_handle;
      // The view may not yet be in a window when create_context fires from
      // platform_create_appwindow - fall back to mainScreen if so.
      CGFloat scale = view.window ? view.window.backingScaleFactor
                                   : NSScreen.mainScreen.backingScaleFactor;
      if (scale <= 0) scale = 1.0;
      st->backing_scale = static_cast<float>(scale);
      st->dpi           = static_cast<uint32_t>(96.0f * st->backing_scale + 0.5f);
    }
    return st;
  }

  static void cg_destroy_context(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st) return;
    if (st->path) { CGPathRelease(st->path); st->path = nullptr; }
    delete st;
  }

  static void cg_resize(neui_render_ctx_t raw, uint32_t width, uint32_t height)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st) return;
    st->width  = static_cast<float>(width);
    st->height = static_cast<float>(height);
  }

  static void cg_begin_frame(neui_render_ctx_t raw, uint32_t clear_argb)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx) return;
    // Pair with cg_end_frame's restore - keeps clip / transform changes from
    // leaking past one frame even if a widget forgot a pop.
    CGContextSaveGState(st->cg_ctx);
    st->alpha_stack.clear();
    st->font_stack.clear();
    CGFloat rgba[4];
    argb_to_rgba(clear_argb, rgba);
    CGContextSetRGBFillColor(st->cg_ctx, rgba[0], rgba[1], rgba[2], rgba[3]);
    CGContextFillRect(st->cg_ctx, CGRectMake(0, 0, st->width, st->height));
  }

  static void cg_end_frame(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx) return;
    CGContextRestoreGState(st->cg_ctx);
    // The CGContextRef only lives for the duration of AppKit's drawRect:.
    // Drop the pointer so no later call accidentally draws into a freed context.
    st->cg_ctx = nullptr;
  }

  static void cg_fill_rect(neui_render_ctx_t raw,
                            float x, float y, float w, float h,
                            uint32_t argb)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx) return;
    CGFloat rgba[4]; argb_to_rgba(argb, rgba, current_alpha(st));
    CGContextSetRGBFillColor(st->cg_ctx, rgba[0], rgba[1], rgba[2], rgba[3]);
    CGContextFillRect(st->cg_ctx, CGRectMake(x, y, w, h));
  }

  static void cg_draw_rect(neui_render_ctx_t raw,
                            float x, float y, float w, float h,
                            float stroke_width, uint32_t argb)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx) return;
    CGFloat rgba[4]; argb_to_rgba(argb, rgba, current_alpha(st));
    CGContextSetRGBStrokeColor(st->cg_ctx, rgba[0], rgba[1], rgba[2], rgba[3]);
    CGContextSetLineWidth(st->cg_ctx, stroke_width);
    CGContextStrokeRect(st->cg_ctx, CGRectMake(x, y, w, h));
  }

  static float cg_get_scale_factor(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CGContextState*>(raw);
    return st ? st->backing_scale : 1.0f;
  }

  static void cg_update_dpi(neui_render_ctx_t raw, uint32_t dpi)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st) return;
    st->dpi           = dpi;
    st->backing_scale = static_cast<float>(dpi) / 96.0f;
  }

  // Draw UTF-8 text into (x, y, w, h) in logical pixels.
  // Vertical centering (matches d2d's PARAGRAPH_ALIGNMENT_CENTER), left-aligned
  // horizontally - widget paint code that wants centered/right-aligned text
  // pre-computes the x via measure_text and passes the adjusted origin, same
  // shape as on Windows.
  //
  // Y-flip note: the view is isFlipped=YES, so the CTM has Y inverted. CoreText
  // glyphs draw upward from the baseline in user space, which would render
  // upside-down under an inverted CTM. We counter-flip locally around the
  // baseline so glyphs come out the right way up.
  static void cg_draw_text(neui_render_ctx_t raw,
                            float x, float y, float w, float h,
                            const char* text, float font_size,
                            uint32_t argb)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx || !text || !*text || font_size <= 0.0f) return;

    CTLineRef line = make_ctline(st, text, -1, font_size);
    if (!line) return;

    CGFloat ascent = 0, descent = 0, leading = 0;
    CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    CGFloat line_height = ascent + descent + leading;
    CGFloat text_y_top  = y + (h - line_height) * 0.5f;
    if (text_y_top < y) text_y_top = y;
    CGFloat baseline_y  = text_y_top + ascent;

    CGContextRef cg = st->cg_ctx;
    CGContextSaveGState(cg);

    // Clip to the requested rect - matches D2D_DRAW_TEXT_OPTIONS_CLIP and
    // keeps over-long strings from bleeding into adjacent widgets.
    CGContextClipToRect(cg, CGRectMake(x, y, w, h));

    CGFloat rgba[4]; argb_to_rgba(argb, rgba, current_alpha(st));
    CGContextSetRGBFillColor(cg, rgba[0], rgba[1], rgba[2], rgba[3]);

    // Origin -> baseline; counter-flip Y so glyphs draw upright.
    CGContextTranslateCTM(cg, x, baseline_y);
    CGContextScaleCTM(cg, 1.0, -1.0);
    CGContextSetTextPosition(cg, 0, 0);
    CTLineDraw(line, cg);

    CGContextRestoreGState(cg);
    CFRelease(line);
  }

  static float cg_measure_text(neui_render_ctx_t raw,
                                const char* text, int text_len,
                                float font_size)
  {
    if (!text || !*text || font_size <= 0.0f) return 0.0f;
    auto* st = static_cast<CGContextState*>(raw);
    CTLineRef line = make_ctline(st, text, text_len, font_size);
    if (!line) return 0.0f;
    CGFloat ascent = 0, descent = 0, leading = 0;
    double width = CTLineGetTypographicBounds(line, &ascent, &descent, &leading);
    CFRelease(line);
    return static_cast<float>(width);
  }

  static void cg_push_clip(neui_render_ctx_t raw,
                            float x, float y, float w, float h)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx) return;
    CGContextSaveGState(st->cg_ctx);
    CGContextClipToRect(st->cg_ctx, CGRectMake(x, y, w, h));
  }

  static void cg_pop_clip(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx) return;
    CGContextRestoreGState(st->cg_ctx);
  }

  // CGImage-backed bitmap handle. width_px/height_px are the physical
  // dimensions of the image; scale is the HiDPI factor (1.0 = @1x). The
  // logical width is width_px/scale, mirroring how d2d_backend sets the
  // bitmap's DPI to scale*96 to expose logical sizes through GetSize().
  struct CGBitmapHandle
  {
    CGImageRef image     = nullptr;
    uint32_t   width_px  = 0;
    uint32_t   height_px = 0;
    float      scale     = 1.0f;
  };

  static void* cg_create_bitmap(neui_render_ctx_t /*raw*/,
                                 uint32_t width_px, uint32_t height_px,
                                 const uint8_t* bgra_pixels, float scale)
  {
    if (!bgra_pixels || width_px == 0 || height_px == 0) return nullptr;
    if (scale <= 0.0f) scale = 1.0f;

    size_t total = static_cast<size_t>(width_px) * height_px * 4;
    // Copy the bytes - caller's buffer may be transient.
    uint8_t* owned = new uint8_t[total];
    memcpy(owned, bgra_pixels, total);

    // CGDataProviderCreateWithData hands ownership of the buffer to the
    // provider; the release callback runs when the last CGImage that
    // references the provider goes away.
    CGDataProviderRef prov = CGDataProviderCreateWithData(
      owned, owned, total,
      [](void* info, const void* /*data*/, size_t /*size*/) {
        delete[] static_cast<uint8_t*>(info);
      });
    if (!prov) {
      delete[] owned;
      return nullptr;
    }

    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGImageRef img = CGImageCreate(
      width_px, height_px, /*bpc*/8, /*bpp*/32, /*row*/width_px * 4u, cs,
      kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little,
      prov, NULL, /*interp*/false, kCGRenderingIntentDefault);
    CGColorSpaceRelease(cs);
    CGDataProviderRelease(prov);  // image retains provider while alive

    if (!img) return nullptr;

    auto* h = new CGBitmapHandle();
    h->image     = img;
    h->width_px  = width_px;
    h->height_px = height_px;
    h->scale     = scale;
    return h;
  }

  static void cg_destroy_bitmap(neui_render_ctx_t /*raw*/, void* bitmap)
  {
    if (!bitmap) return;
    auto* h = static_cast<CGBitmapHandle*>(bitmap);
    if (h->image) CGImageRelease(h->image);
    delete h;
  }

  // Draw a bitmap region into the dst rect. Coordinates are logical pixels.
  // src_w / src_h <= 0 means "entire bitmap"; otherwise the (src_x..) rect
  // is interpreted in the bitmap's logical-coordinate space and converted
  // to physical pixels (x scale) before sub-imaging.
  //
  // The view is isFlipped=YES, so the CTM has Y inverted. CGContextDrawImage
  // treats the image's natural origin as bottom-left in user space, which
  // would flip the bitmap visually under our CTM. Counter-flip locally
  // around the dst rect so images render right-side-up.
  static void cg_draw_bitmap(neui_render_ctx_t raw, void* bitmap,
                              float src_x, float src_y, float src_w, float src_h,
                              float dst_x, float dst_y, float dst_w, float dst_h)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx || !bitmap) return;
    auto* h = static_cast<CGBitmapHandle*>(bitmap);
    if (!h->image || dst_w <= 0.0f || dst_h <= 0.0f) return;

    CGImageRef draw_img    = h->image;
    bool       owns_subimg = false;
    if (src_w > 0.0f && src_h > 0.0f) {
      CGRect src_phys = CGRectMake(src_x * h->scale, src_y * h->scale,
                                    src_w * h->scale, src_h * h->scale);
      CGImageRef sub = CGImageCreateWithImageInRect(h->image, src_phys);
      if (!sub) return;
      draw_img    = sub;
      owns_subimg = true;
    }

    CGContextSaveGState(st->cg_ctx);
    CGContextSetAlpha(st->cg_ctx, current_alpha(st));
    CGContextTranslateCTM(st->cg_ctx, dst_x, dst_y + dst_h);
    CGContextScaleCTM(st->cg_ctx, 1.0, -1.0);
    CGContextDrawImage(st->cg_ctx, CGRectMake(0, 0, dst_w, dst_h), draw_img);
    CGContextRestoreGState(st->cg_ctx);

    if (owns_subimg) CGImageRelease(draw_img);
  }

  // ---------------------------------------------------------------------------
  // Path API

  static void cg_begin_path(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st) return;
    if (st->path) { CGPathRelease(st->path); st->path = nullptr; }
    st->path   = CGPathCreateMutable();
    st->cursor = CGPointZero;
  }

  static void cg_move_to(neui_render_ctx_t raw, float x, float y)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->path) return;
    CGPathMoveToPoint(st->path, NULL, x, y);
    st->cursor = CGPointMake(x, y);
  }

  static void cg_line_to(neui_render_ctx_t raw, float x, float y)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->path) return;
    CGPathAddLineToPoint(st->path, NULL, x, y);
    st->cursor = CGPointMake(x, y);
  }

  static void cg_arc(neui_render_ctx_t raw,
                      float cx, float cy, float radius,
                      float start_rad, float end_rad)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->path) return;
    // The view is isFlipped=YES, so the CTM has Y mirrored. CGPathAddArc's
    // "clockwise" parameter is in path-space (pre-transform). With the flip,
    // a path-space CCW sweep paints CW visually. So a positive sweep
    // (end > start, which renderer.h documents as "clockwise on screen")
    // maps to clockwise=NO; a negative sweep maps to clockwise=YES.
    bool clockwise_path_space = (end_rad < start_rad);
    CGPathAddArc(st->path, NULL, cx, cy, radius, start_rad, end_rad,
                  clockwise_path_space);
    st->cursor = CGPointMake(cx + radius * cosf(end_rad),
                              cy + radius * sinf(end_rad));
  }

  static void cg_close_path(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->path) return;
    CGPathCloseSubpath(st->path);
  }

  static void cg_fill_path(neui_render_ctx_t raw, uint32_t argb)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx || !st->path) return;
    CGFloat rgba[4]; argb_to_rgba(argb, rgba, current_alpha(st));
    CGContextSetRGBFillColor(st->cg_ctx, rgba[0], rgba[1], rgba[2], rgba[3]);
    CGContextAddPath(st->cg_ctx, st->path);
    CGContextFillPath(st->cg_ctx);
  }

  static void cg_stroke_path(neui_render_ctx_t raw,
                              float stroke_width, uint32_t argb)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx || !st->path) return;
    CGFloat rgba[4]; argb_to_rgba(argb, rgba, current_alpha(st));
    CGContextSetRGBStrokeColor(st->cg_ctx, rgba[0], rgba[1], rgba[2], rgba[3]);
    CGContextSetLineWidth(st->cg_ctx, stroke_width);
    CGContextAddPath(st->cg_ctx, st->path);
    CGContextStrokePath(st->cg_ctx);
  }

  // ---------------------------------------------------------------------------
  // Transform stack - CGContextSaveGState/RestoreGState saves+restores both
  // the CTM and the clip stack atomically. Widget code never interleaves
  // push_clip / push_transform pairs, so a single save/restore stack is fine.

  static void cg_push_transform(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx) return;
    CGContextSaveGState(st->cg_ctx);
  }

  static void cg_pop_transform(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx) return;
    CGContextRestoreGState(st->cg_ctx);
  }

  static void cg_translate(neui_render_ctx_t raw, float dx, float dy)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx) return;
    CGContextTranslateCTM(st->cg_ctx, dx, dy);
  }

  static void cg_rotate(neui_render_ctx_t raw, float radians)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx) return;
    // CGContextRotateCTM rotates positive CCW in user space. Under our
    // isFlipped=YES CTM that becomes CW visually, matching renderer.h's
    // "Y axis is screen-down, positive sweep is clockwise" convention.
    CGContextRotateCTM(st->cg_ctx, radians);
  }

  static void cg_scale(neui_render_ctx_t raw, float sx, float sy)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->cg_ctx) return;
    CGContextScaleCTM(st->cg_ctx, sx, sy);
  }

  // CoreGraphics contexts are recreated per draw cycle via set_current_frame
  // and never silently invalidate the way a D3D-backed D2D target can, so
  // there's no generation to advance. Cached target-bound resources are
  // already keyed by the per-frame CGContextRef, so a constant satisfies
  // the asset manager's stale-cache check.
  static uint32_t cg_get_context_generation(neui_render_ctx_t)
  {
    return 0u;
  }

  // ---------------------------------------------------------------------------
  // Alpha stack - pure software stack. Alpha is multiplied into each draw's
  // colour at the call site (and into draw_bitmap via CGContextSetAlpha
  // inside its already-existing SaveGState bracket), so we don't rely on
  // CGContextSetAlpha as a global state - which would be saved/restored by
  // an interleaving transform/clip push and break the alpha-stack invariant.

  static void cg_push_alpha(neui_render_ctx_t raw, float factor)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st) return;
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    float prev = st->alpha_stack.empty() ? 1.0f : st->alpha_stack.back();
    st->alpha_stack.push_back(prev * factor);
  }

  static void cg_pop_alpha(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || st->alpha_stack.empty()) return;
    st->alpha_stack.pop_back();
  }

  // ---------------------------------------------------------------------------
  // Font stack - selects the (family, weight) pair feeding draw_text /
  // measure_text. Mirror of d2d_push_font / d2d_pop_font; font_size stays a
  // per-call parameter and the stack resets on every begin_frame. Empty
  // family => system UI font; weight is CSS-style 100..900 (0 = Regular).

  static void cg_push_font(neui_render_ctx_t raw, const char* family_utf8, int weight)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st) return;
    FontState fs;
    if (family_utf8 && *family_utf8) fs.family = family_utf8;
    fs.weight = weight;
    st->font_stack.push_back(std::move(fs));
  }

  static void cg_pop_font(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || st->font_stack.empty()) return;
    st->font_stack.pop_back();
  }

  // ---------------------------------------------------------------------------

  static neui_render_backend_t backend = {
    NEUI_VERSION,
    cg_create_context,
    cg_destroy_context,
    cg_resize,
    cg_begin_frame,
    cg_end_frame,
    cg_fill_rect,
    cg_draw_rect,
    cg_get_scale_factor,
    cg_update_dpi,
    cg_draw_text,
    cg_measure_text,
    cg_push_clip,
    cg_pop_clip,
    cg_create_bitmap,
    cg_destroy_bitmap,
    cg_draw_bitmap,
    cg_begin_path,
    cg_move_to,
    cg_line_to,
    cg_arc,
    cg_close_path,
    cg_fill_path,
    cg_stroke_path,
    cg_push_transform,
    cg_pop_transform,
    cg_translate,
    cg_rotate,
    cg_scale,
    cg_get_context_generation,
    cg_push_alpha,
    cg_pop_alpha,
    cg_push_font,
    cg_pop_font,
  };

  neui_render_backend_t* get_backend() { return &backend; }

} // namespace neui_cg_backend
