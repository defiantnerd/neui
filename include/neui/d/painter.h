#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"
#include "assets.h"

// Painter API - curated drawing surface handed to NEUI_W_CUSTOMDRAW
// widgets during NEUI_EVENT_WIDGET_PAINT. The painter forwards to the
// active render backend, but exposes ONLY the draw-safe subset: shapes,
// path, state stack, queries, and asset drawing. Context lifecycle
// (begin_frame / end_frame / create_context / resize / update_dpi) and
// raw bitmap creation are NOT reachable through the painter - those are
// host-internal concerns, and exposing them to client paint callbacks
// would be a footgun (a client calling begin_frame would clear the
// surface mid-frame, etc.).
//
// Origin (0, 0) is the widget's top-left in logical pixels at 96 DPI.
// Colours are 0xAARRGGBB. The framework wraps the WIDGET_PAINT dispatch
// in push_transform / push_clip(widget bounds) / pop_clip /
// pop_transform so a client that forgets one pop can't corrupt
// neighbouring widgets.
//
// Threading: paint events fire on the UI thread; the painter is valid
// only for the duration of the WIDGET_PAINT callback.

#ifdef __cplusplus
extern "C" {
#endif

  // Opaque handle bundling the active render backend + ctx for this
  // paint call, plus the bookkeeping required to resolve neui_asset_t
  // handles. Constructed by the host inside the WIDGET_PAINT dispatch
  // site; the client receives a pointer and passes it into every
  // painter_api call.
  typedef struct neui_painter neui_painter_t;

  typedef struct neui_painter_api {
    uint32_t neui_version;

    // ---- Read-only queries -------------------------------------------------

    // HiDPI factor of the underlying surface (1.0, 1.25, 1.5, 2.0, ...).
    // Logical-pixel coordinates ARE already scaled to physical by the
    // backend; this is mostly useful for hairline-stroke alignment.
    float (NEUI_ABI *get_scale_factor)(neui_painter_t* p);

    // Measure UTF-8 text width in logical pixels. text_len = -1 for the
    // full string. Result is paint-state-independent (no transform).
    float (NEUI_ABI *measure_text)(neui_painter_t* p,
                                    const char* utf8, int text_len,
                                    float font_size);

    // ---- Shapes ------------------------------------------------------------

    void (NEUI_ABI *fill_rect)(neui_painter_t* p,
                                float x, float y, float w, float h,
                                uint32_t argb);

    void (NEUI_ABI *draw_rect)(neui_painter_t* p,
                                float x, float y, float w, float h,
                                float stroke_width, uint32_t argb);

    // Draw UTF-8 text clipped to the given rectangle. font_size is in
    // logical pixels.
    void (NEUI_ABI *draw_text)(neui_painter_t* p,
                                float x, float y, float w, float h,
                                const char* utf8,
                                float font_size, uint32_t argb);

    // ---- Path API (stateful within a single paint call) --------------------
    // Build a path with begin_path -> move_to / line_to / arc / close_path,
    // then fill_path or stroke_path to commit it. The path state resets
    // on the next begin_path.

    void (NEUI_ABI *begin_path) (neui_painter_t* p);
    void (NEUI_ABI *move_to)    (neui_painter_t* p, float x, float y);
    void (NEUI_ABI *line_to)    (neui_painter_t* p, float x, float y);
    // Arc sweep is implicit from a_start -> a_end direction (matches the
    // underlying backend's arc primitive). For a reverse-direction sweep,
    // swap the endpoints.
    void (NEUI_ABI *arc)        (neui_painter_t* p,
                                  float cx, float cy, float r,
                                  float a_start, float a_end);
    void (NEUI_ABI *close_path) (neui_painter_t* p);
    void (NEUI_ABI *fill_path)  (neui_painter_t* p, uint32_t argb);
    void (NEUI_ABI *stroke_path)(neui_painter_t* p, float stroke_width,
                                  uint32_t argb);

    // ---- State stack -------------------------------------------------------
    // Axis-aligned clip stack. Stacks must be balanced inside the paint
    // call; the framework adds its own outer brackets, so leftover pushes
    // affect at most the current widget (until the framework pops).

    void (NEUI_ABI *push_clip)(neui_painter_t* p,
                                float x, float y, float w, float h);
    void (NEUI_ABI *pop_clip) (neui_painter_t* p);

    // 2x3 affine transform stack. push captures the current transform;
    // pop restores it. translate / rotate / scale apply to the top of
    // the stack (post-multiply, i.e. local-to-parent semantics).
    void (NEUI_ABI *push_transform)(neui_painter_t* p);
    void (NEUI_ABI *pop_transform) (neui_painter_t* p);
    void (NEUI_ABI *translate)     (neui_painter_t* p, float dx, float dy);
    void (NEUI_ABI *rotate)        (neui_painter_t* p, float radians);
    void (NEUI_ABI *scale)         (neui_painter_t* p, float sx, float sy);

    // ---- Assets ------------------------------------------------------------

    // Draw the asset at handle `asset` aspect-fitted into (x, y, w, h)
    // in logical pixels. No-op if the handle is invalid or the asset
    // couldn't be loaded. For bitmaps the per-context GPU upload happens
    // lazily on first draw and is cached for the lifetime of the
    // backend context.
    void (NEUI_ABI *draw_asset)(neui_painter_t* p, neui_asset_t asset,
                                  float x, float y, float w, float h);

    // ---- Alpha stack -------------------------------------------------------
    // Multiplies a 0..1 opacity factor into every subsequent draw until
    // popped. Pushes compose multiplicatively (pushing 0.5 under 0.5 yields
    // an effective 0.25). Pairs must be balanced inside the paint call;
    // the framework adds no outer alpha bracket of its own.
    void (NEUI_ABI *push_alpha)(neui_painter_t* p, float factor);
    void (NEUI_ABI *pop_alpha) (neui_painter_t* p);

    // ---- Font stack --------------------------------------------------------
    // Push a (family, weight) pair onto the font stack. Every subsequent
    // draw_text / measure_text resolves the active family + weight from
    // the top of the stack and combines it with the call's font_size to
    // pick the backend text format. NULL or "" family = host system
    // default; weight 0 = Normal (CSS 400). Pairs must be balanced inside
    // the paint call; the framework adds no outer font bracket of its own.
    void (NEUI_ABI *push_font)(neui_painter_t* p,
                                const char* family_utf8,
                                int          weight);
    void (NEUI_ABI *pop_font) (neui_painter_t* p);

    // ---- Filmstrip assets --------------------------------------------------
    // (Vtable-appended; check the api version / pointer before calling.)
    //
    // Draw cell `frame` of a filmstrip / stitchmap asset (a bitmap tagged via
    // neui_asset_api::set_frame_layout) aspect-fitted into (x, y, w, h). The
    // whole strip uploads to the GPU once; each frame is a sub-rect sample of
    // that single upload, so animating the frame is cheap. `frame` clamps into
    // [0, frame_count), so a value past the end pins to the last frame. On an
    // asset with no frame layout this draws the whole bitmap (== draw_asset).
    void (NEUI_ABI *draw_asset_frame)(neui_painter_t* p, neui_asset_t asset,
                                       uint32_t frame,
                                       float x, float y, float w, float h);
  } neui_painter_api_t;

#ifdef __cplusplus
}
#endif
