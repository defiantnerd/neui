#include "null_backend.h"

namespace neui_null_backend
{
  static neui_render_ctx_t null_create_context(void*, uint32_t, uint32_t)
  {
    return nullptr;
  }

  static void null_destroy_context(neui_render_ctx_t) {}
  static void null_resize(neui_render_ctx_t, uint32_t, uint32_t) {}
  static void null_begin_frame(neui_render_ctx_t, uint32_t) {}
  static void null_end_frame(neui_render_ctx_t) {}
  static void  null_fill_rect      (neui_render_ctx_t, float, float, float, float, uint32_t) {}
  static void  null_draw_rect      (neui_render_ctx_t, float, float, float, float, float, uint32_t) {}
  static float null_get_scale_factor(neui_render_ctx_t) { return 1.0f; }
  static void  null_update_dpi     (neui_render_ctx_t, uint32_t) {}
  static void  null_draw_text      (neui_render_ctx_t, float, float, float, float,
                                    const char*, float, uint32_t) {}
  static float null_measure_text   (neui_render_ctx_t, const char*, int, float) { return 0.0f; }
  static void  null_push_clip      (neui_render_ctx_t, float, float, float, float) {}
  static void  null_pop_clip       (neui_render_ctx_t) {}
  static void* null_create_bitmap  (neui_render_ctx_t, uint32_t, uint32_t, const uint8_t*, float) { return nullptr; }
  static void  null_destroy_bitmap (neui_render_ctx_t, void*) {}
  static void  null_draw_bitmap    (neui_render_ctx_t, void*, float, float, float, float, float, float, float, float) {}
  static void  null_begin_path     (neui_render_ctx_t) {}
  static void  null_move_to        (neui_render_ctx_t, float, float) {}
  static void  null_line_to        (neui_render_ctx_t, float, float) {}
  static void  null_arc            (neui_render_ctx_t, float, float, float, float, float) {}
  static void  null_close_path     (neui_render_ctx_t) {}
  static void  null_fill_path      (neui_render_ctx_t, uint32_t) {}
  static void  null_stroke_path    (neui_render_ctx_t, float, uint32_t) {}
  static void  null_push_transform (neui_render_ctx_t) {}
  static void  null_pop_transform  (neui_render_ctx_t) {}
  static void  null_translate      (neui_render_ctx_t, float, float) {}
  static void  null_rotate         (neui_render_ctx_t, float) {}
  static void  null_scale          (neui_render_ctx_t, float, float) {}
  static uint32_t null_get_context_generation(neui_render_ctx_t) { return 0u; }
  static void  null_push_alpha     (neui_render_ctx_t, float) {}
  static void  null_pop_alpha      (neui_render_ctx_t) {}
  static void  null_push_font      (neui_render_ctx_t, const char*, int) {}
  static void  null_pop_font       (neui_render_ctx_t) {}
  static neui_render_ctx_t null_create_offscreen_context(uint32_t, uint32_t, float) { return nullptr; }
  static bool  null_read_pixels_bgra(neui_render_ctx_t, uint8_t*) { return false; }

  static neui_render_backend_t backend = {
    NEUI_VERSION,
    null_create_context,
    null_destroy_context,
    null_resize,
    null_begin_frame,
    null_end_frame,
    null_fill_rect,
    null_draw_rect,
    null_get_scale_factor,
    null_update_dpi,
    null_draw_text,
    null_measure_text,
    null_push_clip,
    null_pop_clip,
    null_create_bitmap,
    null_destroy_bitmap,
    null_draw_bitmap,
    null_begin_path,
    null_move_to,
    null_line_to,
    null_arc,
    null_close_path,
    null_fill_path,
    null_stroke_path,
    null_push_transform,
    null_pop_transform,
    null_translate,
    null_rotate,
    null_scale,
    null_get_context_generation,
    null_push_alpha,
    null_pop_alpha,
    null_push_font,
    null_pop_font,
    null_create_offscreen_context,
    null_read_pixels_bgra,
  };

  neui_render_backend_t* get_backend() { return &backend; }

} // namespace neui_null_backend
