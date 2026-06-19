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

// The CG backend serves both macOS (AppKit) and iOS (UIKit). CoreGraphics +
// CoreText are identical on both; only the font-weight constant table, the
// native-handle view type, and the screen-scale query differ. Those three
// spots are wrapped in TARGET_OS_IPHONE conditionals below.
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE
#import <UIKit/UIKit.h>
#else
#import <AppKit/AppKit.h>
#endif
#import <CoreGraphics/CoreGraphics.h>
#import <CoreText/CoreText.h>

#include <cmath>
#include <cstring>
#include <string>
#include <unordered_map>
#include <vector>

#include "cg_backend.h"
#include "../shared/backend_util.h"

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

    // Off-screen-specific. When `surface_pixels` is non-null this ctx
    // owns its CGContextRef + the BGRA buffer behind it (created via
    // cg_create_offscreen_context); cg_ctx stays bound for the ctx's
    // lifetime instead of being rebound per drawRect: via
    // set_current_frame. read_pixels_bgra walks `surface_pixels`.
    uint8_t* surface_pixels = nullptr;
    uint32_t surface_w_px   = 0;
    uint32_t surface_h_px   = 0;
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

  static inline void argb_to_rgba(uint32_t argb, CGFloat out[4], float alpha_mul = 1.0f)
  {
    neui_detail::argb_unpack(argb, out, alpha_mul);
  }

  static inline float current_alpha(const CGContextState* st)
  {
    return neui_detail::alpha_stack_current(st->alpha_stack);
  }

  // CTFont cache - keyed by "family|weight|size_q10". Process-wide and never
  // evicted; the number of distinct (family, weight, size) tuples used in
  // typical apps is tiny (parallels d2d_backend's IDWriteTextFormat cache).
  static std::unordered_map<std::string, CTFontRef>& font_cache()
  {
    static std::unordered_map<std::string, CTFontRef> cache;
    return cache;
  }

  // CSS-style weight (100..900, 0 = unset) -> platform font-weight scale.
  // Mirror of the d2d backend's normalise_weight mapping. AppKit's
  // NSFontWeight* and UIKit's UIFontWeight* constants are both CGFloat scales
  // over the same range, so only the constant names differ between platforms.
  static CGFloat css_weight_to_nsfontweight(int weight)
  {
#if TARGET_OS_IPHONE
    if (weight <= 0)  return UIFontWeightRegular;
    if (weight < 150) return UIFontWeightUltraLight; // 100
    if (weight < 250) return UIFontWeightThin;       // 200
    if (weight < 350) return UIFontWeightLight;      // 300
    if (weight < 450) return UIFontWeightRegular;    // 400
    if (weight < 550) return UIFontWeightMedium;     // 500
    if (weight < 650) return UIFontWeightSemibold;   // 600
    if (weight < 750) return UIFontWeightBold;       // 700
    if (weight < 850) return UIFontWeightHeavy;      // 800
    return UIFontWeightBlack;                         // 900
#else
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
#endif
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
    key += std::to_string(neui_detail::font_size_q10(font_size));

    auto& cache = font_cache();
    auto it = cache.find(key);
    if (it != cache.end()) return it->second;

    CGFloat ns_weight = css_weight_to_nsfontweight(weight);
#if TARGET_OS_IPHONE
    UIFont* font = nil;
    if (family.empty()) {
      // SF Pro on modern iOS, at the requested weight.
      font = [UIFont systemFontOfSize:static_cast<CGFloat>(font_size)
                               weight:ns_weight];
    } else {
      NSString* fam = [NSString stringWithUTF8String:family.c_str()];
      if (fam) {
        UIFontDescriptor* desc = [UIFontDescriptor fontDescriptorWithFontAttributes:@{
          UIFontDescriptorFamilyAttribute : fam,
          UIFontDescriptorTraitsAttribute : @{ UIFontWeightTrait : @(ns_weight) },
        }];
        font = [UIFont fontWithDescriptor:desc size:static_cast<CGFloat>(font_size)];
      }
      if (!font)  // unknown family -> graceful fallback to the system font
        font = [UIFont systemFontOfSize:static_cast<CGFloat>(font_size)
                                 weight:ns_weight];
    }
#else
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
#endif
    // NSFont / UIFont is toll-free bridged with CTFont; retain a +1 ref for the cache.
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

    // kCTForegroundColorFromContextAttributeName = true makes CTLineDraw use
    // the CGContext's fill colour (set by cg_draw_text from the argb arg).
    // Without it Core Text defaults every glyph to black and ignores the
    // requested colour.
    CFStringRef keys[]   = { kCTFontAttributeName,
                             kCTForegroundColorFromContextAttributeName };
    CFTypeRef   values[] = { font, kCFBooleanTrue };
    CFDictionaryRef attrs = CFDictionaryCreate(
      kCFAllocatorDefault,
      reinterpret_cast<const void**>(keys),
      reinterpret_cast<const void**>(values),
      2,
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
#if TARGET_OS_IPHONE
      UIView* view = (__bridge UIView*)native_handle;
      // The view may not yet be in a window when create_context fires from
      // platform_create_appwindow - fall back to the main screen if so.
      CGFloat scale = (view.window && view.window.screen)
                        ? view.window.screen.scale
                        : UIScreen.mainScreen.scale;
#else
      NSView* view = (__bridge NSView*)native_handle;
      // The view may not yet be in a window when create_context fires from
      // platform_create_appwindow - fall back to mainScreen if so.
      CGFloat scale = view.window ? view.window.backingScaleFactor
                                   : NSScreen.mainScreen.backingScaleFactor;
#endif
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
    // For off-screen ctxs we own cg_ctx + the pixel buffer; for window
    // ctxs AppKit owns cg_ctx and it's already been cleared in cg_end_frame.
    if (st->surface_pixels) {
      if (st->cg_ctx) CGContextRelease(st->cg_ctx);
      free(st->surface_pixels);
      st->surface_pixels = nullptr;
      st->cg_ctx         = nullptr;
    }
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
    // For window ctxs the CGContextRef only lives for the duration of
    // AppKit's drawRect:; drop the pointer so no later call draws into a
    // freed context. Off-screen ctxs own their CGContextRef for life -
    // keep it bound so a follow-up read_pixels_bgra (and a subsequent
    // begin_frame for re-render) work.
    if (!st->surface_pixels)
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
  // `tint` is an ARGB multiplicative colour. 0xFFFFFFFFu = passthrough
  // (untinted byte-for-byte draw); any other value first clips to the
  // bitmap's alpha shape (CGContextClipToMask), then fills the masked area
  // with the tint colour under kCGBlendModeMultiply so the source pixels
  // come out multiplied by the tint. Matches the D2D1Tint effect's
  // semantics for premultiplied BGRA bitmaps.
  //
  // The view is isFlipped=YES, so the CTM has Y inverted. CGContextDrawImage
  // treats the image's natural origin as bottom-left in user space, which
  // would flip the bitmap visually under our CTM. Counter-flip locally
  // around the dst rect so images render right-side-up.
  static void cg_draw_bitmap(neui_render_ctx_t raw, void* bitmap,
                              float src_x, float src_y, float src_w, float src_h,
                              float dst_x, float dst_y, float dst_w, float dst_h,
                              uint32_t tint)
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

    if (tint == 0xFFFFFFFFu) {
      // Untinted fast path - byte-for-byte identical to pre-tint behaviour.
      CGContextDrawImage(st->cg_ctx, CGRectMake(0, 0, dst_w, dst_h), draw_img);
    } else {
      // Tinted path - replicate the D2D1Tint composite as closely as CG
      // blend modes allow. Inside an isolated transparency layer: draw the
      // image at full alpha, clip to its alpha shape, multiply-fill the
      // tint RGB at FULL strength (alpha 1 - tinting strength must not be
      // diluted by the tint's own alpha or the alpha stack). The layer
      // then composites onto the destination at (tint alpha x alpha
      // stack), which is how D2D1Tint scales the image's alpha channel -
      // a translucent tint lets the background bleed through rather than
      // mixing the original image colours back in.
      //
      // Residual divergence vs D2D (documented, accepted): D2D1Tint with
      // ClampOutput=FALSE multiplies premultiplied channels, so for
      // tint alpha < 1 its colour term stays at full premul strength
      // (superluminous); CG clamps colours to the layer alpha, so deeply
      // translucent tints render slightly darker here. Identical for
      // opaque tints (0xFFrrggbb), which is the dominant use.
      CGRect rect = CGRectMake(0, 0, dst_w, dst_h);
      CGFloat rgba[4];
      argb_to_rgba(tint, rgba);
      CGContextSetAlpha(st->cg_ctx, current_alpha(st) * rgba[3]);
      CGContextBeginTransparencyLayerWithRect(st->cg_ctx, rect, NULL);
      CGContextDrawImage(st->cg_ctx, rect, draw_img);
      CGContextClipToMask(st->cg_ctx, rect, draw_img);
      CGContextSetBlendMode(st->cg_ctx, kCGBlendModeMultiply);
      CGContextSetRGBFillColor(st->cg_ctx, rgba[0], rgba[1], rgba[2], 1.0);
      CGContextFillRect(st->cg_ctx, rect);
      CGContextEndTransparencyLayer(st->cg_ctx);
    }
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
    neui_detail::alpha_stack_push(st->alpha_stack, factor);
  }

  static void cg_pop_alpha(neui_render_ctx_t raw)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st) return;
    neui_detail::alpha_stack_pop(st->alpha_stack);
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
  // Off-screen contexts (NEUI_ASSET_KIND_SURFACE).

  static neui_render_ctx_t cg_create_offscreen_context(uint32_t width_px,
                                                         uint32_t height_px,
                                                         float    scale)
  {
    if (width_px == 0 || height_px == 0) return nullptr;
    if (scale <= 0.0f) scale = 1.0f;

    size_t row_bytes = static_cast<size_t>(width_px) * 4u;
    size_t total     = row_bytes * height_px;
    uint8_t* pixels = static_cast<uint8_t*>(calloc(1, total));
    if (!pixels) return nullptr;

    // kCGImageAlphaPremultipliedFirst + kCGBitmapByteOrder32Little =
    // memory order B, G, R, A - matches the BGRA8 premul layout used
    // everywhere else (asset pixels, clipboard images, the win32 WIC
    // path).
    CGColorSpaceRef cs = CGColorSpaceCreateWithName(kCGColorSpaceSRGB);
    CGContextRef cg = CGBitmapContextCreate(
      pixels, width_px, height_px,
      /*bpc*/8, row_bytes, cs,
      kCGImageAlphaPremultipliedFirst | kCGBitmapByteOrder32Little);
    CGColorSpaceRelease(cs);
    if (!cg) {
      free(pixels);
      return nullptr;
    }

    auto* st = new CGContextState();
    st->cg_ctx         = cg;
    st->surface_pixels = pixels;
    st->surface_w_px   = width_px;
    st->surface_h_px   = height_px;
    st->backing_scale  = scale;
    st->dpi            = static_cast<uint32_t>(scale * 96.0f + 0.5f);
    st->width          = static_cast<float>(width_px) / scale;   // logical
    st->height         = static_cast<float>(height_px) / scale;

    // Logical-pixel input + Y-down: scale by `scale` so a logical-pixel
    // draw maps to (scale)x physical pixels, then translate + flip Y so
    // the origin is top-left (matching isFlipped=YES on window views and
    // the renderer.h Y-down convention). Apply once at create-time; the
    // begin_frame Save/Restore bracket preserves this baseline CTM
    // across every frame the surface is repainted.
    CGContextScaleCTM(cg, scale, scale);
    CGContextTranslateCTM(cg, 0.0, st->height);
    CGContextScaleCTM(cg, 1.0, -1.0);
    return st;
  }

  static bool cg_read_pixels_bgra(neui_render_ctx_t raw, uint8_t* out_bgra)
  {
    auto* st = static_cast<CGContextState*>(raw);
    if (!st || !st->surface_pixels || !out_bgra) return false;
    size_t total = static_cast<size_t>(st->surface_w_px) * st->surface_h_px * 4u;
    memcpy(out_bgra, st->surface_pixels, total);
    return true;
  }

  // ---------------------------------------------------------------------------
  // Client-registered fonts (NEUI_ASSET_KIND_FONT).
  //
  // Registration is process-scoped via CTFontManager, which makes the family
  // resolvable both for the painted path (get_active_font's NSFont/UIFont
  // descriptor lookup, unchanged) AND for the native control host's
  // NSFont/UIFont fontWithName: - so the macOS/iOS native prong comes for
  // free. Unknown families still fall back, so this is additive.

  struct AppFontEntry {
    uint64_t  token   = 0;
    CGFontRef cg_font = nullptr;  // memory form (retained; unregistered on release)
    CFURLRef  url     = nullptr;  // file form (retained)
    bool      is_file = false;
  };
  static std::vector<AppFontEntry>& app_fonts()
  {
    static std::vector<AppFontEntry> v;
    return v;
  }
  static uint64_t g_next_font_token = 1;

  static void copy_family(CFStringRef famcf, char* out_family, uint32_t cap)
  {
    if (!out_family || cap == 0) return;
    out_family[0] = '\0';
    if (!famcf) return;
    char buf[256] = { 0 };
    if (CFStringGetCString(famcf, buf, sizeof(buf), kCFStringEncodingUTF8)) {
      uint32_t n = static_cast<uint32_t>(std::strlen(buf));
      if (n > cap - 1) n = cap - 1;
      if (n) std::memcpy(out_family, buf, n);
      out_family[n] = '\0';
    }
  }

  static bool cg_register_font(const uint8_t* data, uint32_t len,
                                char* out_family, uint32_t cap,
                                uint64_t* out_token)
  {
    if (out_family && cap) out_family[0] = '\0';
    if (out_token) *out_token = 0;
    if (!data || len == 0) return false;

    // Provider does not own the bytes (callback null) - the asset store keeps
    // them alive for the token's lifetime and unregister runs before they go.
    CGDataProviderRef dp =
      CGDataProviderCreateWithData(nullptr, data, len, nullptr);
    if (!dp) return false;
    CGFontRef font = CGFontCreateWithDataProvider(dp);
    CGDataProviderRelease(dp);
    if (!font) return false;

    CFErrorRef err = nullptr;
    if (!CTFontManagerRegisterGraphicsFont(font, &err)) {
      if (err) CFRelease(err);
      CGFontRelease(font);
      return false;
    }

    // Family name via a transient CTFont over the CGFont.
    CTFontRef   ctf   = CTFontCreateWithGraphicsFont(font, 12.0, nullptr, nullptr);
    CFStringRef famcf = ctf ? CTFontCopyFamilyName(ctf) : nullptr;
    copy_family(famcf, out_family, cap);
    if (famcf) CFRelease(famcf);
    if (ctf)   CFRelease(ctf);

    AppFontEntry e;
    e.token   = g_next_font_token++;
    e.cg_font = font;       // retained until unregister
    e.is_file = false;
    app_fonts().push_back(e);
    if (out_token) *out_token = e.token;
    return true;
  }

  static bool cg_register_font_file(const char* path,
                                     char* out_family, uint32_t cap,
                                     uint64_t* out_token)
  {
    if (out_family && cap) out_family[0] = '\0';
    if (out_token) *out_token = 0;
    if (!path || !*path) return false;

    CFStringRef cfpath =
      CFStringCreateWithCString(nullptr, path, kCFStringEncodingUTF8);
    if (!cfpath) return false;
    CFURLRef url = CFURLCreateWithFileSystemPath(nullptr, cfpath,
                                                  kCFURLPOSIXPathStyle, false);
    CFRelease(cfpath);
    if (!url) return false;

    CFErrorRef err = nullptr;
    if (!CTFontManagerRegisterFontsForURL(url, kCTFontManagerScopeProcess,
                                           &err)) {
      if (err) CFRelease(err);
      CFRelease(url);
      return false;
    }

    // Family name from the URL's first descriptor.
    CFArrayRef descs = CTFontManagerCreateFontDescriptorsFromURL(url);
    if (descs && CFArrayGetCount(descs) > 0) {
      CTFontDescriptorRef d =
        (CTFontDescriptorRef)CFArrayGetValueAtIndex(descs, 0);
      CFStringRef famcf =
        (CFStringRef)CTFontDescriptorCopyAttribute(d, kCTFontFamilyNameAttribute);
      copy_family(famcf, out_family, cap);
      if (famcf) CFRelease(famcf);
    }
    if (descs) CFRelease(descs);

    AppFontEntry e;
    e.token   = g_next_font_token++;
    e.url     = url;        // retained until unregister
    e.is_file = true;
    app_fonts().push_back(e);
    if (out_token) *out_token = e.token;
    return true;
  }

  static void cg_unregister_font(uint64_t token)
  {
    if (!token) return;
    auto& v = app_fonts();
    for (auto it = v.begin(); it != v.end(); ++it) {
      if (it->token != token) continue;
      CFErrorRef err = nullptr;
      if (it->is_file) {
        if (it->url) {
          CTFontManagerUnregisterFontsForURL(it->url,
                                             kCTFontManagerScopeProcess, &err);
          CFRelease(it->url);
        }
      } else if (it->cg_font) {
        CTFontManagerUnregisterGraphicsFont(it->cg_font, &err);
        CGFontRelease(it->cg_font);
      }
      if (err) CFRelease(err);
      v.erase(it);
      return;
    }
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
    cg_create_offscreen_context,
    cg_read_pixels_bgra,
    cg_register_font,
    cg_register_font_file,
    cg_unregister_font,
  };

  neui_render_backend_t* get_backend() { return &backend; }

} // namespace neui_cg_backend
