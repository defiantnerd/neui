#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <d2d1.h>
#include <dwrite.h>
#include <cstdint>
#include <cmath>
#include <unordered_map>
#include <vector>

#include "d2d_backend.h"

#pragma comment(lib, "d2d1")
#pragma comment(lib, "dwrite")

namespace neui_d2d_backend
{
  // Per-window render context.
  struct D2DContext
  {
    ID2D1HwndRenderTarget* target      = nullptr;
    ID2D1SolidColorBrush*  brush       = nullptr;
    uint32_t               dpi         = 96;

    // Re-create state. The hwnd + size are stashed at create_context time
    // and kept in sync by d2d_resize / d2d_update_dpi. If D2D returns
    // D2DERR_RECREATE_TARGET from EndDraw the target is gone but the
    // context pointer stays valid, so the next begin_frame can rebuild
    // the target in place. `generation` is bumped whenever the target is
    // recreated; clients that cache target-bound resources (ID2D1Bitmap
    // handles) check this to discover that their caches are stale.
    HWND                   hwnd        = nullptr;
    uint32_t               width       = 0;
    uint32_t               height      = 0;
    uint32_t               generation  = 1;

    // Path API state. The path is constructed via move_to/line_to/arc;
    // figure_open tracks whether BeginFigure has been called without a
    // matching EndFigure. fill_path / stroke_path close the figure and the
    // sink, then draw. The path stays valid for additional fill/stroke
    // calls until the next begin_path.
    ID2D1PathGeometry*     path        = nullptr;
    ID2D1GeometrySink*     sink        = nullptr;
    bool                   figure_open = false;
    bool                   sink_closed = false;
    D2D1_POINT_2F          cursor_pt   = { 0.0f, 0.0f };

    // Transform stack. `current` is the active D2D world transform, applied
    // via ID2D1RenderTarget::SetTransform whenever it changes. push pushes
    // a copy of `current` onto the stack; pop restores it. Reset to
    // identity on every begin_frame.
    D2D1::Matrix3x2F            current{ D2D1::Matrix3x2F::Identity() };
    std::vector<D2D1::Matrix3x2F> transform_stack;
  };

  // Process-wide D2D factory - created once, never destroyed (lives for process lifetime).
  static ID2D1Factory*   g_factory        = nullptr;
  static IDWriteFactory* g_dwrite_factory = nullptr;

  // Text format cache - keyed by font_size * 10 (rounded to 0.1pt), value is IDWriteTextFormat*.
  // Entries are never evicted; the number of distinct sizes used in a typical app is tiny.
  static std::unordered_map<uint32_t, IDWriteTextFormat*> g_text_format_cache;

  static bool ensure_factory()
  {
    if (g_factory) return true;
    HRESULT hr = D2D1CreateFactory(D2D1_FACTORY_TYPE_SINGLE_THREADED, &g_factory);
    if (FAILED(hr)) return false;
    hr = DWriteCreateFactory(DWRITE_FACTORY_TYPE_SHARED,
                             __uuidof(IDWriteFactory),
                             reinterpret_cast<IUnknown**>(&g_dwrite_factory));
    return SUCCEEDED(hr);
  }

  // Returns a cached IDWriteTextFormat for the given logical font size, creating it if needed.
  // Returns nullptr on failure.
  static IDWriteTextFormat* get_text_format(float font_size)
  {
    if (!g_dwrite_factory) return nullptr;
    uint32_t key = static_cast<uint32_t>(font_size * 10.0f + 0.5f);
    auto it = g_text_format_cache.find(key);
    if (it != g_text_format_cache.end()) return it->second;

    IDWriteTextFormat* fmt = nullptr;
    HRESULT hr = g_dwrite_factory->CreateTextFormat(
      L"Segoe UI",
      nullptr,
      DWRITE_FONT_WEIGHT_NORMAL,
      DWRITE_FONT_STYLE_NORMAL,
      DWRITE_FONT_STRETCH_NORMAL,
      font_size,
      L"",
      &fmt
    );
    if (FAILED(hr)) return nullptr;
    fmt->SetWordWrapping(DWRITE_WORD_WRAPPING_NO_WRAP);
    fmt->SetParagraphAlignment(DWRITE_PARAGRAPH_ALIGNMENT_CENTER);
    g_text_format_cache[key] = fmt;
    return fmt;
  }

  // Logical pixel (96 DPI base) to physical pixel conversion.
  static float l2p(float logical, uint32_t dpi)
  {
    return logical * static_cast<float>(dpi) / 96.0f;
  }

  static D2D1_COLOR_F argb_to_color(uint32_t argb)
  {
    return D2D1::ColorF(
      ((argb >> 16) & 0xFF) / 255.0f,
      ((argb >>  8) & 0xFF) / 255.0f,
      ( argb        & 0xFF) / 255.0f,
      ((argb >> 24) & 0xFF) / 255.0f
    );
  }

  // ---------------------------------------------------------------------------

  // Build target + brush for the context's stored hwnd / size / dpi.
  // Used by both d2d_create_context and the device-loss recovery path in
  // d2d_begin_frame; populates ctx->target and ctx->brush on success and
  // leaves them null on failure. Does not touch ctx->generation - callers
  // bump that when they want the recreation to be visible to bitmap caches.
  static bool d2d_build_target(D2DContext* ctx)
  {
    if (!ctx || !ctx->hwnd || ctx->width == 0 || ctx->height == 0) return false;
    if (!ensure_factory()) return false;

    auto props = D2D1::RenderTargetProperties();
    props.dpiX = static_cast<float>(ctx->dpi);
    props.dpiY = static_cast<float>(ctx->dpi);

    auto hwnd_props = D2D1::HwndRenderTargetProperties(
      ctx->hwnd,
      D2D1::SizeU(ctx->width, ctx->height)
    );

    ID2D1HwndRenderTarget* target = nullptr;
    HRESULT hr = g_factory->CreateHwndRenderTarget(props, hwnd_props, &target);
    if (FAILED(hr)) return false;

    target->SetDpi(static_cast<float>(ctx->dpi), static_cast<float>(ctx->dpi));

    ID2D1SolidColorBrush* brush = nullptr;
    hr = target->CreateSolidColorBrush(D2D1::ColorF(D2D1::ColorF::White), &brush);
    if (FAILED(hr)) { target->Release(); return false; }

    ctx->target = target;
    ctx->brush  = brush;
    return true;
  }

  static neui_render_ctx_t d2d_create_context(void* native_handle,
                                               uint32_t width, uint32_t height)
  {
    if (!ensure_factory()) return nullptr;

    HWND hwnd = reinterpret_cast<HWND>(native_handle);
    UINT dpi  = GetDpiForWindow(hwnd);

    auto* ctx  = new D2DContext();
    ctx->hwnd   = hwnd;
    ctx->width  = width;
    ctx->height = height;
    ctx->dpi    = dpi;
    if (!d2d_build_target(ctx)) { delete ctx; return nullptr; }
    return ctx;
  }

  static void d2d_destroy_context(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    if (ctx->sink)   { ctx->sink->Release();   ctx->sink   = nullptr; }
    if (ctx->path)   { ctx->path->Release();   ctx->path   = nullptr; }
    if (ctx->brush)  { ctx->brush->Release();  ctx->brush  = nullptr; }
    if (ctx->target) { ctx->target->Release(); ctx->target = nullptr; }
    delete ctx;
  }

  static void d2d_resize(neui_render_ctx_t raw, uint32_t width, uint32_t height)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    ctx->width  = width;
    ctx->height = height;
    if (ctx->target) ctx->target->Resize(D2D1::SizeU(width, height));
    // If target is null we're in the lost-device window between EndDraw
    // returning D2DERR_RECREATE_TARGET and the next begin_frame; the new
    // size is already stashed and will be used when begin_frame rebuilds.
  }

  static void d2d_begin_frame(neui_render_ctx_t raw, uint32_t clear_argb)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    if (!ctx->target) {
      // Previous frame's EndDraw lost the device. Try to rebuild in place
      // and bump generation so cached target-bound bitmaps get re-uploaded
      // on first use. If rebuild fails (e.g. driver still in a bad state)
      // we silently skip this frame; the next begin_frame will try again.
      if (!d2d_build_target(ctx)) return;
      ctx->generation++;
    }
    ctx->target->BeginDraw();
    // Reset transform stack to identity each frame so a missing pop in
    // a previous frame can't bleed across frame boundaries.
    ctx->current = D2D1::Matrix3x2F::Identity();
    ctx->transform_stack.clear();
    ctx->target->SetTransform(ctx->current);
    ctx->target->Clear(argb_to_color(clear_argb));
  }

  static void d2d_end_frame(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target) return;
    HRESULT hr = ctx->target->EndDraw();
    if (hr == D2DERR_RECREATE_TARGET) {
      // Device lost (mode change, GPU reset, driver crash, ...). Release
      // target + brush; ID2D1Bitmap handles created against this target
      // are now dangling for draw purposes but still safe to Release().
      // We don't touch them here - the asset manager (one per session)
      // owns the cache and will drop stale entries the next time it sees
      // a different get_context_generation result. The next begin_frame
      // rebuilds the target using the stored hwnd / size / dpi.
      if (ctx->target) { ctx->target->Release(); ctx->target = nullptr; }
      if (ctx->brush)  { ctx->brush->Release();  ctx->brush  = nullptr; }
    }
  }

  static uint32_t d2d_get_context_generation(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    return ctx ? ctx->generation : 0u;
  }

  static void d2d_fill_rect(neui_render_ctx_t raw,
                             float x, float y, float w, float h,
                             uint32_t argb)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !ctx->brush) return;
    ctx->brush->SetColor(argb_to_color(argb));
    // Coordinates are logical (96 DPI); D2D target already has DPI set, so pass as-is.
    ctx->target->FillRectangle(D2D1::RectF(x, y, x + w, y + h), ctx->brush);
  }

  static void d2d_draw_rect(neui_render_ctx_t raw,
                             float x, float y, float w, float h,
                             float stroke_width,
                             uint32_t argb)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !ctx->brush) return;
    ctx->brush->SetColor(argb_to_color(argb));
    ctx->target->DrawRectangle(D2D1::RectF(x, y, x + w, y + h), ctx->brush, stroke_width);
  }

  static float d2d_get_scale_factor(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return 1.0f;
    return static_cast<float>(ctx->dpi) / 96.0f;
  }

  static void d2d_update_dpi(neui_render_ctx_t raw, uint32_t dpi)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    ctx->dpi = dpi;
    // Mirror the new DPI onto the live target if present; if we're in the
    // lost-device window the rebuild in d2d_begin_frame will use the
    // stashed value.
    if (ctx->target)
      ctx->target->SetDpi(static_cast<float>(dpi), static_cast<float>(dpi));
  }

  static void d2d_draw_text(neui_render_ctx_t raw,
                             float x, float y, float w, float h,
                             const char* text,
                             float font_size,
                             uint32_t argb)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !ctx->brush || !text || !*text) return;

    IDWriteTextFormat* fmt = get_text_format(font_size);
    if (!fmt) return;

    // Convert UTF-8 to UTF-16.
    int needed = MultiByteToWideChar(CP_UTF8, 0, text, -1, nullptr, 0);
    if (needed <= 1) return;  // empty or error
    wchar_t  stack_buf[256];
    wchar_t* wbuf = (needed <= 256) ? stack_buf : new wchar_t[needed];
    MultiByteToWideChar(CP_UTF8, 0, text, -1, wbuf, needed);
    int len = needed - 1;  // exclude null terminator

    ctx->brush->SetColor(argb_to_color(argb));
    D2D1_RECT_F rect = D2D1::RectF(x, y, x + w, y + h);
    ctx->target->DrawText(
      wbuf, static_cast<UINT32>(len),
      fmt,
      rect,
      ctx->brush,
      D2D1_DRAW_TEXT_OPTIONS_CLIP
    );

    if (wbuf != stack_buf) delete[] wbuf;
  }

  static float d2d_measure_text(neui_render_ctx_t /*raw*/,
                                 const char* text, int text_len,
                                 float font_size)
  {
    if (!g_dwrite_factory || !text || !*text) return 0.0f;
    IDWriteTextFormat* fmt = get_text_format(font_size);
    if (!fmt) return 0.0f;

    // Convert UTF-8 → UTF-16, honouring text_len byte limit.
    int byte_len = (text_len < 0) ? -1 : text_len;
    int needed = MultiByteToWideChar(CP_UTF8, 0, text, byte_len, nullptr, 0);
    if (needed <= 0) return 0.0f;
    // MultiByteToWideChar with byte_len >= 0 does NOT append a null terminator.
    wchar_t  stack_buf[256];
    wchar_t* wbuf = (needed <= 256) ? stack_buf : new wchar_t[needed];
    MultiByteToWideChar(CP_UTF8, 0, text, byte_len, wbuf, needed);

    IDWriteTextLayout* layout = nullptr;
    HRESULT hr = g_dwrite_factory->CreateTextLayout(
      wbuf, static_cast<UINT32>(needed),
      fmt,
      100000.0f, 100000.0f,   // very large max - we want unconstrained width
      &layout
    );

    if (wbuf != stack_buf) delete[] wbuf;
    if (FAILED(hr) || !layout) return 0.0f;

    DWRITE_TEXT_METRICS metrics = {};
    layout->GetMetrics(&metrics);
    layout->Release();
    return metrics.widthIncludingTrailingWhitespace;
  }

  static void* d2d_create_bitmap(neui_render_ctx_t raw,
                                  uint32_t width_px, uint32_t height_px,
                                  const uint8_t* bgra_pixels,
                                  float scale)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !bgra_pixels || width_px == 0 || height_px == 0)
      return nullptr;

    // Set the bitmap's DPI to scale*96 so that GetSize() returns logical dimensions
    // (physical_px / scale) and src coordinates in draw calls use logical pixels.
    float bmp_dpi = scale * 96.0f;
    D2D1_BITMAP_PROPERTIES props = {};
    props.pixelFormat = D2D1::PixelFormat(DXGI_FORMAT_B8G8R8A8_UNORM,
                                           D2D1_ALPHA_MODE_PREMULTIPLIED);
    props.dpiX = bmp_dpi;
    props.dpiY = bmp_dpi;

    ID2D1Bitmap* bmp = nullptr;
    UINT32 pitch = width_px * 4;
    HRESULT hr = ctx->target->CreateBitmap(
      D2D1::SizeU(width_px, height_px),
      bgra_pixels, pitch,
      &props,
      &bmp);
    return SUCCEEDED(hr) ? static_cast<void*>(bmp) : nullptr;
  }

  static void d2d_destroy_bitmap(neui_render_ctx_t /*raw*/, void* bitmap)
  {
    if (bitmap)
      static_cast<ID2D1Bitmap*>(bitmap)->Release();
  }

  static void d2d_draw_bitmap(neui_render_ctx_t raw, void* bitmap,
                               float src_x, float src_y, float src_w, float src_h,
                               float dst_x, float dst_y, float dst_w, float dst_h)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !bitmap) return;

    auto* bmp = static_cast<ID2D1Bitmap*>(bitmap);

    D2D1_SIZE_F logical_size = bmp->GetSize(); // DIPs = logical px (bitmap DPI is set)
    D2D1_RECT_F src_rect;
    if (src_w <= 0.0f || src_h <= 0.0f) {
      src_rect = D2D1::RectF(0.0f, 0.0f, logical_size.width, logical_size.height);
    } else {
      src_rect = D2D1::RectF(src_x, src_y, src_x + src_w, src_y + src_h);
    }

    D2D1_RECT_F dst_rect = D2D1::RectF(dst_x, dst_y, dst_x + dst_w, dst_y + dst_h);

    ctx->target->DrawBitmap(bmp, dst_rect, 1.0f,
                             D2D1_BITMAP_INTERPOLATION_MODE_LINEAR,
                             src_rect);
  }

  static void d2d_push_clip(neui_render_ctx_t raw, float x, float y, float w, float h)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target) return;
    ctx->target->PushAxisAlignedClip(D2D1::RectF(x, y, x + w, y + h),
                                     D2D1_ANTIALIAS_MODE_ALIASED);
  }

  static void d2d_pop_clip(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target) return;
    ctx->target->PopAxisAlignedClip();
  }

  // ---------------------------------------------------------------------------
  // Path API

  // Internal: release any open path/sink (called from begin_path and from
  // the implicit teardown if a path is left around when the context dies).
  static void release_path(D2DContext* ctx)
  {
    if (ctx->sink) {
      if (ctx->figure_open) {
        ctx->sink->EndFigure(D2D1_FIGURE_END_OPEN);
        ctx->figure_open = false;
      }
      if (!ctx->sink_closed) {
        ctx->sink->Close();
      }
      ctx->sink->Release();
      ctx->sink = nullptr;
    }
    if (ctx->path) { ctx->path->Release(); ctx->path = nullptr; }
    ctx->sink_closed = false;
  }

  // Internal: end the figure (if open) and close the sink (if not already
  // closed). After this the path geometry is finalised and can be drawn
  // multiple times via FillGeometry / DrawGeometry, but no more segments
  // can be appended (a new begin_path is required for that).
  static void finalise_path(D2DContext* ctx)
  {
    if (!ctx->sink) return;
    if (ctx->figure_open) {
      ctx->sink->EndFigure(D2D1_FIGURE_END_OPEN);
      ctx->figure_open = false;
    }
    if (!ctx->sink_closed) {
      ctx->sink->Close();
      ctx->sink_closed = true;
    }
  }

  static void d2d_begin_path(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target) return;

    release_path(ctx);

    ID2D1PathGeometry* path = nullptr;
    if (FAILED(g_factory->CreatePathGeometry(&path))) return;
    ID2D1GeometrySink* sink = nullptr;
    if (FAILED(path->Open(&sink))) { path->Release(); return; }

    ctx->path        = path;
    ctx->sink        = sink;
    ctx->figure_open = false;
    ctx->sink_closed = false;
    ctx->cursor_pt   = D2D1::Point2F(0.0f, 0.0f);
  }

  static void d2d_move_to(neui_render_ctx_t raw, float x, float y)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->sink || ctx->sink_closed) return;

    if (ctx->figure_open) {
      ctx->sink->EndFigure(D2D1_FIGURE_END_OPEN);
      ctx->figure_open = false;
    }
    D2D1_POINT_2F p = D2D1::Point2F(x, y);
    ctx->sink->BeginFigure(p, D2D1_FIGURE_BEGIN_FILLED);
    ctx->figure_open = true;
    ctx->cursor_pt   = p;
  }

  static void d2d_line_to(neui_render_ctx_t raw, float x, float y)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->sink || ctx->sink_closed) return;

    if (!ctx->figure_open) {
      ctx->sink->BeginFigure(ctx->cursor_pt, D2D1_FIGURE_BEGIN_FILLED);
      ctx->figure_open = true;
    }
    D2D1_POINT_2F p = D2D1::Point2F(x, y);
    ctx->sink->AddLine(p);
    ctx->cursor_pt = p;
  }

  // Append one D2D arc segment. Caller has already opened the figure.
  static void d2d_add_arc_segment(D2DContext* ctx,
                                   float cx, float cy, float radius,
                                   float start_rad, float end_rad)
  {
    D2D1_POINT_2F end = D2D1::Point2F(cx + radius * cosf(end_rad),
                                       cy + radius * sinf(end_rad));
    float sweep = end_rad - start_rad;
    D2D1_SWEEP_DIRECTION dir =
      (sweep >= 0.0f) ? D2D1_SWEEP_DIRECTION_CLOCKWISE
                      : D2D1_SWEEP_DIRECTION_COUNTER_CLOCKWISE;
    float abs_sweep = sweep < 0.0f ? -sweep : sweep;
    D2D1_ARC_SIZE size =
      (abs_sweep > 3.14159265f) ? D2D1_ARC_SIZE_LARGE : D2D1_ARC_SIZE_SMALL;

    D2D1_ARC_SEGMENT seg = {};
    seg.point          = end;
    seg.size           = D2D1::SizeF(radius, radius);
    seg.rotationAngle  = 0.0f;
    seg.sweepDirection = dir;
    seg.arcSize        = size;
    ctx->sink->AddArc(seg);
    ctx->cursor_pt = end;
  }

  static void d2d_arc(neui_render_ctx_t raw,
                       float cx, float cy, float radius,
                       float start_rad, float end_rad)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->sink || ctx->sink_closed) return;

    D2D1_POINT_2F start = D2D1::Point2F(cx + radius * cosf(start_rad),
                                         cy + radius * sinf(start_rad));

    if (!ctx->figure_open) {
      ctx->sink->BeginFigure(start, D2D1_FIGURE_BEGIN_FILLED);
      ctx->figure_open = true;
    } else {
      // Bridge from current cursor to the arc start.
      ctx->sink->AddLine(start);
    }

    // Direct2D's AddArc renders nothing when the start point and end point
    // coincide, so a full-circle arc (|sweep| ≈ 2π) produces no output.
    // Split such arcs into two halves through the antipodal point so each
    // segment has distinct endpoints.
    const float PI     = 3.14159265358979323846f;
    const float TWO_PI = 6.28318530717958647692f;
    float sweep = end_rad - start_rad;
    float abs_sweep = sweep < 0.0f ? -sweep : sweep;
    if (abs_sweep + 1e-4f >= TWO_PI) {
      float mid_rad = start_rad + (sweep >= 0.0f ? PI : -PI);
      d2d_add_arc_segment(ctx, cx, cy, radius, start_rad, mid_rad);
      d2d_add_arc_segment(ctx, cx, cy, radius, mid_rad,   end_rad);
    } else {
      d2d_add_arc_segment(ctx, cx, cy, radius, start_rad, end_rad);
    }
  }

  static void d2d_close_path(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->sink || ctx->sink_closed) return;

    if (ctx->figure_open) {
      ctx->sink->EndFigure(D2D1_FIGURE_END_CLOSED);
      ctx->figure_open = false;
    }
  }

  static void d2d_fill_path(neui_render_ctx_t raw, uint32_t argb)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !ctx->brush || !ctx->path) return;

    finalise_path(ctx);
    ctx->brush->SetColor(argb_to_color(argb));
    ctx->target->FillGeometry(ctx->path, ctx->brush);
  }

  static void d2d_stroke_path(neui_render_ctx_t raw, float stroke_width, uint32_t argb)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || !ctx->target || !ctx->brush || !ctx->path) return;

    finalise_path(ctx);
    ctx->brush->SetColor(argb_to_color(argb));
    ctx->target->DrawGeometry(ctx->path, ctx->brush, stroke_width);
  }

  // ---------------------------------------------------------------------------
  // Transform stack

  static void d2d_push_transform(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    ctx->transform_stack.push_back(ctx->current);
  }

  static void d2d_pop_transform(neui_render_ctx_t raw)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx || ctx->transform_stack.empty()) return;
    ctx->current = ctx->transform_stack.back();
    ctx->transform_stack.pop_back();
    if (ctx->target) ctx->target->SetTransform(ctx->current);
  }

  static void d2d_translate(neui_render_ctx_t raw, float dx, float dy)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    // Post-multiply: child translation applies inside the parent's frame.
    ctx->current = D2D1::Matrix3x2F::Translation(dx, dy) * ctx->current;
    if (ctx->target) ctx->target->SetTransform(ctx->current);
  }

  static void d2d_rotate(neui_render_ctx_t raw, float radians)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    // D2D's Matrix3x2F::Rotation takes degrees, not radians.
    float deg = radians * (180.0f / 3.14159265358979323846f);
    ctx->current = D2D1::Matrix3x2F::Rotation(deg, D2D1::Point2F(0.0f, 0.0f)) * ctx->current;
    if (ctx->target) ctx->target->SetTransform(ctx->current);
  }

  static void d2d_scale(neui_render_ctx_t raw, float sx, float sy)
  {
    auto* ctx = static_cast<D2DContext*>(raw);
    if (!ctx) return;
    ctx->current = D2D1::Matrix3x2F::Scale(sx, sy, D2D1::Point2F(0.0f, 0.0f)) * ctx->current;
    if (ctx->target) ctx->target->SetTransform(ctx->current);
  }

  // ---------------------------------------------------------------------------

  static neui_render_backend_t backend = {
    NEUI_VERSION,
    d2d_create_context,
    d2d_destroy_context,
    d2d_resize,
    d2d_begin_frame,
    d2d_end_frame,
    d2d_fill_rect,
    d2d_draw_rect,
    d2d_get_scale_factor,
    d2d_update_dpi,
    d2d_draw_text,
    d2d_measure_text,
    d2d_push_clip,
    d2d_pop_clip,
    d2d_create_bitmap,
    d2d_destroy_bitmap,
    d2d_draw_bitmap,
    d2d_begin_path,
    d2d_move_to,
    d2d_line_to,
    d2d_arc,
    d2d_close_path,
    d2d_fill_path,
    d2d_stroke_path,
    d2d_push_transform,
    d2d_pop_transform,
    d2d_translate,
    d2d_rotate,
    d2d_scale,
    d2d_get_context_generation,
  };

  neui_render_backend_t* get_backend() { return &backend; }

} // namespace neui_d2d_backend
