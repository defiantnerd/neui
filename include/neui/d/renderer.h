#pragma once
#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

// Opaque render context - one per native frame window (APPWINDOW or PLUGWINDOW).
// Created by the backend on window creation; owned and destroyed by the host.
typedef void* neui_render_ctx_t;

// Rendering backend interface.
// Backends implement this struct and register with the crossplatform host at link time.
// The native_handle passed to create_context is platform-specific:
//   Win32:  HWND
//   macOS:  NSView*
//   Linux:  Window (X11) or wl_surface* (Wayland)
// All coordinate parameters are logical pixels at 96 DPI baseline.
// Colour parameters are 0xAARRGGBB.
typedef struct neui_render_backend {
  uint32_t neui_version;

  // Create a render context bound to a native window handle.
  neui_render_ctx_t (NEUI_ABI *create_context)(void*    native_handle,
                                                uint32_t width,
                                                uint32_t height);

  // Destroy a context and release all associated backend resources.
  void (NEUI_ABI *destroy_context)(neui_render_ctx_t ctx);

  // Notify the backend that the window has been resized.
  void (NEUI_ABI *resize)(neui_render_ctx_t ctx, uint32_t width, uint32_t height);

  // Begin a frame. Clears the surface to clear_argb.
  void (NEUI_ABI *begin_frame)(neui_render_ctx_t ctx, uint32_t clear_argb);

  // End the frame and present / flush.
  void (NEUI_ABI *end_frame)(neui_render_ctx_t ctx);

  // Fill a rectangle with a solid colour.
  void (NEUI_ABI *fill_rect)(neui_render_ctx_t ctx,
                              float x, float y, float w, float h,
                              uint32_t argb);

  // Draw a rectangle outline.
  void (NEUI_ABI *draw_rect)(neui_render_ctx_t ctx,
                              float x, float y, float w, float h,
                              float stroke_width,
                              uint32_t argb);

  // Returns the current scale factor: physical pixels per logical pixel.
  // 1.0f at 96 DPI, 1.5f at 144 DPI, 2.0f at 192 DPI, etc.
  // Clients may use this to perform their own DPI-aware rendering.
  float (NEUI_ABI *get_scale_factor)(neui_render_ctx_t ctx);

  // Notify the backend that the window's DPI has changed (e.g. moved to a
  // different monitor). dpi is the new DPI value (96 = 100 %).
  // Must be called before the next begin_frame so coordinates map correctly.
  void (NEUI_ABI *update_dpi)(neui_render_ctx_t ctx, uint32_t dpi);

  // Draw UTF-8 text clipped to the given rectangle.
  // font_size is in logical pixels (96 DPI base). argb is 0xAARRGGBB.
  void (NEUI_ABI *draw_text)(neui_render_ctx_t ctx,
                              float x, float y, float w, float h,
                              const char* text,
                              float font_size,
                              uint32_t argb);

  // Measure the rendered width (in logical pixels) of text_len bytes of text
  // at the given font size. Pass text_len = -1 to measure the full string.
  // Returns 0.0f on error or if the backend does not support measurement.
  float (NEUI_ABI *measure_text)(neui_render_ctx_t ctx,
                                  const char* text, int text_len,
                                  float font_size);

  // Push an axis-aligned clip rectangle. All subsequent drawing is clipped to
  // the intersection of this rect and any previously active clip. Must be
  // matched by a corresponding pop_clip call. Calls may be nested.
  void (NEUI_ABI *push_clip)(neui_render_ctx_t ctx,
                              float x, float y, float w, float h);

  // Pop the most recently pushed clip rectangle.
  void (NEUI_ABI *pop_clip)(neui_render_ctx_t ctx);

  // Create an opaque bitmap handle from raw BGRA8 (premultiplied) pixel data.
  // width_px / height_px are physical pixel dimensions.
  // scale is the HiDPI factor of the image (1.0 = @1x, 2.0 = @2x, 3.0 = @3x);
  // the backend uses this to keep logical coordinates consistent with other draw calls.
  // Returns nullptr on failure. The handle is only valid for the given context.
  void* (NEUI_ABI *create_bitmap)(neui_render_ctx_t ctx,
                                   uint32_t width_px, uint32_t height_px,
                                   const uint8_t* bgra_pixels,
                                   float scale);

  // Destroy a bitmap handle previously returned by create_bitmap.
  void (NEUI_ABI *destroy_bitmap)(neui_render_ctx_t ctx, void* bitmap);

  // Draw a bitmap or a sub-region of it.
  // All coordinates are logical pixels at 96 DPI (same as all other draw calls).
  // src_x/y/w/h: source region in the bitmap's logical coordinate space.
  // dst_x/y/w/h: destination rectangle in the render target.
  // Pass src_w == 0 or src_h == 0 to draw the full bitmap.
  void (NEUI_ABI *draw_bitmap)(neui_render_ctx_t ctx, void* bitmap,
                                float src_x, float src_y, float src_w, float src_h,
                                float dst_x, float dst_y, float dst_w, float dst_h);

  // --- Path API (stateful per context) -----------------------------------
  //
  // Usage:
  //   begin_path(ctx);                         // discards any previous path
  //   move_to(ctx, x, y);                      // start a subpath
  //   line_to(ctx, x, y);                      // append line segment
  //   arc(ctx, cx, cy, r, start_rad, end_rad); // append arc segment
  //   close_path(ctx);                         // close current subpath (optional)
  //   fill_path(ctx, argb);                    // fill the path (path stays valid)
  //   stroke_path(ctx, stroke_width, argb);    // stroke the path
  //
  // fill_path and stroke_path may both be called on the same path (e.g. fill
  // the body, then stroke its edge). The path is discarded on the next
  // begin_path call or on destroy_context.
  //
  // Angles are in radians. The Y axis is screen-down, so a positive sweep
  // (start_rad < end_rad) goes clockwise on the rendered surface.
  //
  // All coordinates are logical pixels at 96 DPI.

  void (NEUI_ABI *begin_path) (neui_render_ctx_t ctx);
  void (NEUI_ABI *move_to)    (neui_render_ctx_t ctx, float x, float y);
  void (NEUI_ABI *line_to)    (neui_render_ctx_t ctx, float x, float y);
  void (NEUI_ABI *arc)        (neui_render_ctx_t ctx,
                                float cx, float cy, float radius,
                                float start_rad, float end_rad);
  void (NEUI_ABI *close_path) (neui_render_ctx_t ctx);
  void (NEUI_ABI *fill_path)  (neui_render_ctx_t ctx, uint32_t argb);
  void (NEUI_ABI *stroke_path)(neui_render_ctx_t ctx, float stroke_width,
                                uint32_t argb);

  // --- Transform stack (stateful per context) ----------------------------
  //
  // Usage:
  //   push_transform(ctx);                  // save current transform
  //   translate(ctx, dx, dy);               // post-multiply: t' = t * T
  //   rotate(ctx, theta);                   // post-multiply: t' = t * R(theta)
  //   scale(ctx, sx, sy);                   // post-multiply: t' = t * S
  //   ...draw under transform...
  //   pop_transform(ctx);                   // restore previous transform
  //
  // Transforms are post-multiplied: child transforms apply *inside* parent
  // ones, matching the natural "translate to origin, then rotate around it"
  // pattern. The Y axis is screen-down. Angles are radians.
  //
  // push/pop calls must be balanced. Calling pop_transform with an empty
  // stack is a no-op. The transform is identity at begin_frame.

  void (NEUI_ABI *push_transform)(neui_render_ctx_t ctx);
  void (NEUI_ABI *pop_transform) (neui_render_ctx_t ctx);
  void (NEUI_ABI *translate)     (neui_render_ctx_t ctx, float dx, float dy);
  void (NEUI_ABI *rotate)        (neui_render_ctx_t ctx, float radians);
  void (NEUI_ABI *scale)         (neui_render_ctx_t ctx, float sx, float sy);

} neui_render_backend_t;

#ifdef __cplusplus
}
#endif
