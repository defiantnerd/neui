#pragma once
#include "api.h"
#include "gradient.h"
#include "path_style.h"
#include <stddef.h>   // size_t

#ifdef __cplusplus
extern "C" {
#endif

// Opaque render context - one per native frame window (APPWINDOW or PLUGWINDOW).
// Created by the backend on window creation; owned and destroyed by the host.
typedef void* neui_render_ctx_t;

// A bitmap a textured shader program samples (draw_shader_program_textured):
// premultiplied BGRA8 rows of `stride` bytes. Equal `content_id`s mean equal
// pixels, so a backend may keep one upload per id; 0 = do not keep it. No
// pixels or an empty size is an unbound sampler, which reads as transparent.
typedef struct neui_shader_texture_desc_t {
  const uint8_t* bgra;
  uint32_t width_px, height_px;
  size_t stride;
  uint64_t content_id;
} neui_shader_texture_desc_t;

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

  // --- Internal use only (not reachable through neui_painter_api_t) ------
  //
  // create_bitmap / destroy_bitmap / draw_bitmap operate on raw context-
  // bound handles and are intended for the host's own image / asset
  // pipeline. Clients writing NEUI_W_CUSTOMDRAW widgets get a curated
  // surface through neui_painter_api_t in <neui/d/painter.h>, and load
  // bitmaps via neui_asset_api_t in <neui/d/assets.h> (get_interface
  // (sess, NEUI_API_ASSETS)). The painter resolves neui_asset_t handles
  // against the session's asset manager and forwards to draw_bitmap
  // below - clients never see the void* handle.
  //
  // Calling these from a paint callback would either leak GPU resources
  // (per-frame uploads) or operate on the wrong context entirely.

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
  //
  // `tint` is an ARGB multiplicative colour applied to the bitmap's pixels
  // by the backend. `0xFFFFFFFFu` is the passthrough sentinel: the backend
  // bypasses any effect setup and draws the bitmap byte-for-byte as on
  // pre-tint code paths. Any other value runs the backend's native
  // multiplicative-tint primitive (D2D effect on Windows, blend-mode
  // multiply + alpha mask on macOS); the null backend ignores the tint.
  void (NEUI_ABI *draw_bitmap)(neui_render_ctx_t ctx, void* bitmap,
                                float src_x, float src_y, float src_w, float src_h,
                                float dst_x, float dst_y, float dst_w, float dst_h,
                                uint32_t tint);

  // --- End internal-use block --------------------------------------------

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

  // Monotonically-increasing per-context counter. Bumped whenever the
  // backend has had to recreate its device-dependent state (e.g. D2D's
  // D2DERR_RECREATE_TARGET after a GPU mode change or driver reset).
  // Callers that cache device-dependent handles created via create_bitmap
  // can compare the cached generation against this value to detect that
  // their cached handles have become dangling and need re-upload. The
  // ctx pointer itself remains valid - only the resources hanging off
  // it have been re-issued. Backends that never invalidate (CG, null)
  // return a constant (typically 0).
  uint32_t (NEUI_ABI *get_context_generation)(neui_render_ctx_t ctx);

  // --- Alpha stack (stateful per context) --------------------------------
  //
  // Multiplies a 0..1 opacity factor into every subsequent draw call - text,
  // fill, stroke, path, bitmap. Pushes compose multiplicatively: pushing
  // 0.5 inside an already-pushed 0.5 yields an effective alpha of 0.25.
  // pop_alpha restores the previous top. The stack is reset to empty
  // (effective 1.0) at every begin_frame so a missing pop in one frame
  // can't bleed across.
  //
  // Note: the ARGB alpha byte on individual draws is also honoured;
  // effective = (argb_alpha / 255) * stack_top.
  void (NEUI_ABI *push_alpha)(neui_render_ctx_t ctx, float factor);
  void (NEUI_ABI *pop_alpha) (neui_render_ctx_t ctx);

  // --- Font stack (stateful per context) ---------------------------------
  //
  // Push a (family, weight) pair onto the font stack. Every subsequent
  // draw_text / measure_text call resolves the active family + weight
  // from the top of the stack and combines it with the call's font_size
  // to select a backend text format. Empty family (NULL or "") means
  // "use the host system default" (Segoe UI on the D2D backend); a
  // weight of 0 means Normal (CSS 400). Mapping uses the nearest
  // standard weight bucket per platform.
  //
  // Push / pop pairs must be balanced inside a frame; the stack is
  // reset to empty (effective default) at every begin_frame so a
  // missing pop in one frame can't bleed across.
  //
  // D2D, CG and Cairo implement font selection; the null backend has
  // no-op stubs to keep the vtable shape.
  void (NEUI_ABI *push_font)(neui_render_ctx_t ctx,
                              const char* family_utf8,
                              int          weight);
  void (NEUI_ABI *pop_font) (neui_render_ctx_t ctx);

  // --- Off-screen contexts (backs NEUI_ASSET_KIND_SURFACE) ---------------
  //
  // create_offscreen_context returns a render context that draws into a
  // CPU pixel buffer instead of a native window. Every other backend
  // call (begin_frame / end_frame / fill_rect / draw_text / path /
  // transform / clip / alpha / font) works on the returned ctx exactly
  // as it does on a window ctx. Cleanup is via destroy_context (same
  // path); resize / update_dpi are not supported on off-screen ctxs.
  //
  //   width_px / height_px - physical pixel dimensions of the surface.
  //   scale                - HiDPI factor (1.0 / 2.0 / 3.0); the
  //                          backend sets the ctx DPI so a logical-pixel
  //                          draw call maps to (logical * scale) physical
  //                          pixels, matching how an HWND ctx behaves on
  //                          a (scale)x display.
  //
  // Returns nullptr if the backend does not support off-screen targets
  // (null backend) or on allocation failure.
  neui_render_ctx_t (NEUI_ABI *create_offscreen_context)(
      uint32_t width_px,
      uint32_t height_px,
      float    scale);

  // Read the surface pixels of an off-screen ctx back into out_bgra as
  // BGRA8 premultiplied, top-down, tightly packed (no per-row padding,
  // exactly width_px * 4 bytes per row). out_bgra must point to a
  // buffer of at least width_px * height_px * 4 bytes. Call after
  // end_frame. Returns false on window contexts, on null contexts, or
  // on read failure.
  bool (NEUI_ABI *read_pixels_bgra)(neui_render_ctx_t ctx,
                                      uint8_t*         out_bgra);

  // --- Font registration (backs NEUI_ASSET_KIND_FONT) --------------------
  //
  // Register a client-supplied font so its family becomes resolvable by
  // push_font / draw_text / measure_text. These are FACTORY / PROCESS level,
  // not per-context: registration affects font resolution for every render
  // context the backend serves, so they take no ctx. Font-cache keys
  // (already family + weight + size) need no change; registration only
  // widens name resolution. An unknown family still falls back to the
  // host default, so this is purely additive.

  // Register a font from memory. The backend reads the family name out of
  // the font data and makes that family resolvable. out_family receives the
  // family name (UTF-8, truncated to cap, NUL-terminated). The backend does
  // NOT own `data` - the caller (asset store) keeps the bytes alive for the
  // returned token's lifetime. Writes an opaque token into *out_token for
  // unregister_font and returns true on success; returns false (token 0,
  // empty family) on failure or on backends without font support (null).
  bool (NEUI_ABI *register_font)(const uint8_t* data, uint32_t len,
                                 char* out_family, uint32_t cap,
                                 uint64_t* out_token);

  // Path variant (.ttf / .otf / .ttc). Some backends register URLs / paths
  // more robustly than in-memory bytes. Same contract as register_font.
  bool (NEUI_ABI *register_font_file)(const char* path,
                                      char* out_family, uint32_t cap,
                                      uint64_t* out_token);

  // Best-effort unregister of a previously registered font. A backend may
  // not fully release a face still referenced by a cached text format; that
  // is acceptable - the name stops resolving for new draws. No-op on token 0.
  void (NEUI_ABI *unregister_font)(uint64_t token);

  // --- Gradient fills (vtable-appended) ----------------------------------
  //
  // Fill a rectangle / the current path with the linear or radial colour
  // gradient described by `grad` (see <neui/d/gradient.h>). Gradient
  // geometry is in the same logical-pixel space as the fill region and is
  // subject to the active transform; each stop's ARGB alpha is multiplied
  // by the current alpha-stack top, matching fill_rect / fill_path.
  //
  // A null `grad`, fewer than two stops, or a backend without gradient
  // support (null) draws nothing. Like fill_path, fill_path_gradient leaves
  // the path valid so a subsequent stroke_path / fill_path still works.
  // Coordinates are logical pixels at 96 DPI; colours are 0xAARRGGBB.
  void (NEUI_ABI *fill_rect_gradient)(neui_render_ctx_t ctx,
                                      float x, float y, float w, float h,
                                      const neui_gradient_t* grad);
  void (NEUI_ABI *fill_path_gradient)(neui_render_ctx_t ctx,
                                      const neui_gradient_t* grad);

  // --- Path curves + fill-rule + stroke style (vtable-appended) ----------
  //
  // cubic_to / quad_to append Bézier segments to the current path (like
  // line_to). set_fill_rule selects nonzero / even-odd for the next
  // fill_path / fill_path_gradient; it is path state, resets to NONZERO on
  // begin_path, and must be set before the first path verb (D2D fixes the
  // sink fill mode at begin_path). stroke_path_styled strokes with caps /
  // joins / miter / dashes; a NULL style equals stroke_path. Coordinates are
  // logical pixels at 96 DPI.
  void (NEUI_ABI *cubic_to)(neui_render_ctx_t ctx, float c1x, float c1y,
                            float c2x, float c2y, float x, float y);
  void (NEUI_ABI *quad_to) (neui_render_ctx_t ctx, float cx, float cy,
                            float x, float y);
  void (NEUI_ABI *set_fill_rule)(neui_render_ctx_t ctx, neui_fill_rule_t rule);
  void (NEUI_ABI *stroke_path_styled)(neui_render_ctx_t ctx, float stroke_width,
                                      uint32_t argb,
                                      const neui_stroke_style_t* style);

  // Stroke the current path with a gradient brush (the stroke analogue of
  // fill_path_gradient). NULL style == plain stroke; NULL/<2-stop grad == no-op.
  void (NEUI_ABI *stroke_path_gradient)(neui_render_ctx_t ctx, float stroke_width,
                                        const neui_gradient_t* grad,
                                        const neui_stroke_style_t* style);

  // --- Partial repaint (vtable-appended; NULL = full frames only) ---------
  //
  // Restrict the NEXT begin_frame / end_frame pair to `count` damage rects
  // (x, y, w, h quadruples in logical pixels, frame-local): begin_frame
  // clips to them and clears only there, end_frame presents only them, and
  // every pixel outside keeps the previous frame's content. count == 0 (or
  // rects == NULL) means a full frame. Consumed by that one frame - each
  // frame without a call is full.
  //
  // Returns true only when that frame WILL be partial. A backend may refuse
  // (false = full frame) whenever it can't vouch for the pixels outside the
  // damage - a freshly (re)allocated surface, a DPI change since the last
  // frame. The host must cull widgets only after a true return, otherwise a
  // full clear would wipe what it skipped. A backend that can never keep the
  // previous frame (e.g. a flip-model swap chain without a persistent
  // canvas) leaves this NULL.
  bool (NEUI_ABI *set_frame_damage)(neui_render_ctx_t ctx,
                                    const float* rects_xywh, int count);

  // --- Device mapping (vtable-appended; NULL = untransformed) -------------
  //
  // The current mapping from logical user space to the target's device
  // pixels (y-down, origin at the target's top-left) as an affine matrix:
  //   device_x = m[0] * x + m[2] * y + m[4]
  //   device_y = m[1] * x + m[3] * y + m[5]
  // It includes the display scale and every transform pushed since
  // begin_frame. The host uses it to draw a bitmap 1:1 onto whole device
  // pixels (shader layers). A backend without it is treated as
  // m = { s, 0, 0, s, 0, 0 } with s = get_scale_factor.
  void (NEUI_ABI *get_device_transform)(neui_render_ctx_t ctx, float m[6]);

  // --- GPU shader programs (vtable-appended; NULL = CPU only) ------------
  //
  // Programs are the GPU form of a .nsl shader (the host emits them; the
  // backend only compiles and draws). Without these slots, or when
  // create_shader_program fails, the host renders shaders on the CPU.
  //
  // Compile `src` (`len` bytes) written in `lang` (NEUI_SHADER_LANG_*).
  // Returns an opaque program usable with every ctx of this backend, or NULL
  // when the backend cannot run it (no compiler, a GPU too old). The host
  // calls it once per shader, during a paint, so it must not block: a
  // backend may compile in the background and decline draws (return false)
  // until the program is ready, or for good if it fails to compile.
  void* (NEUI_ABI *create_shader_program)(int lang, const char* src, size_t len);

  // Release a program and everything the backend holds for it in any ctx.
  void  (NEUI_ABI *destroy_shader_program)(void* program);

  // Draw `program` over the device pixels [x, x + w) x [y, y + h) of the
  // ctx's target. The position is in device pixels and ignores the current
  // transform; the clip and the alpha stack apply. `constants` (`count`
  // floats, a multiple of 4) is the program's constant buffer; the backend
  // multiplies its current opacity into constants[NEUI_SHADER_OPACITY_INDEX]
  // (on a copy). Returns false when nothing was drawn, and the host then
  // draws the CPU result instead.
  bool  (NEUI_ABI *draw_shader_program)(neui_render_ctx_t ctx, void* program,
                                        int32_t x, int32_t y, uint32_t w, uint32_t h,
                                        const float* constants, uint32_t count);

  // draw_shader_program for a program that samples bitmaps: tex[i] is the
  // program's sampler i (texture register t<i> in HLSL_D2D), `tex_count` of
  // them. NULL = programs that sample run on the CPU.
  bool  (NEUI_ABI *draw_shader_program_textured)(neui_render_ctx_t ctx, void* program,
                                                 int32_t x, int32_t y, uint32_t w, uint32_t h,
                                                 const float* constants, uint32_t count,
                                                 const neui_shader_texture_desc_t* tex, uint32_t tex_count);

} neui_render_backend_t;

// Languages for create_shader_program. HLSL_D2D: an HLSL ps_4_0 pixel shader
// for a zero-input Direct2D custom effect, entry `main(float4 pos :
// SV_POSITION, float4 scene : SCENE_POSITION) : SV_Target`, constant buffer
// b0; SCENE_POSITION is 0 at the drawn rect's top-left, in device pixels. A
// program that samples reads `Texture2D t<i> : register(t<i>)` with Load,
// one per sampler, bound by draw_shader_program_textured.
#define NEUI_SHADER_LANG_HLSL_D2D 1

// The constant the backend multiplies its opacity into.
#define NEUI_SHADER_OPACITY_INDEX 6

#ifdef __cplusplus
}
#endif
