#pragma once
#include <neui/neui.h>
#include <stdint.h>

// LVGL rendering backend for the crossplatform host (prototype - see
// plans/lvgl-host-approach-c.md). Unlike the window-bound D2D / CG / Cairo
// backends, a context here is not permanently attached to a drawable: LVGL
// hands out a target layer only inside a draw event, so the platform layer
// binds / unbinds the active lv_layer_t around each paint dispatch.

struct _lv_layer_t;  // matches LVGL's typedef struct _lv_layer_t lv_layer_t

namespace neui_lvgl_backend
{
  neui_render_backend_t* get_backend();

  // Not every backend entry point runs inside a draw dispatch: measure_text is
  // called from the host's non-painting sizing paths and destroy_context from
  // window teardown, yet both touch LVGL global state (Tiny TTF instances and
  // their glyph caches, the vector path allocator) that a display thread may be
  // inside under lv_lock. The platform layer owns the lock policy - it alone
  // knows when the current thread is already inside an LVGL callback - so it
  // installs a re-entrant lock / unlock pair here at init. Without them the
  // backend does no locking at all (single-threaded LVGL builds).
  void set_lock_hooks(void (*lock)(), void (*unlock)());

  // Bind the LVGL layer this ctx draws into for the duration of one draw
  // dispatch (LV_EVENT_DRAW_MAIN / DRAW_POST). (base_x, base_y) is the
  // display-coordinate position the ctx's logical origin maps to. Binding
  // resets the transform / clip / alpha / font stacks (the per-frame reset
  // the other backends do in begin_frame).
  void bind_layer(neui_render_ctx_t ctx, _lv_layer_t* layer,
                  int32_t base_x, int32_t base_y);

  // Unbind after the dispatch returns; restores the layer's original clip.
  void unbind_layer(neui_render_ctx_t ctx);

  // Free per-draw deferred resources (sub-image descriptors referenced by
  // LVGL draw tasks). Call once per loop turn AFTER lv_timer_handler has
  // returned - the refresh that consumed the tasks has completed by then.
  // Must NOT be called between draw dispatches of the same refresh (retained
  // mode binds per widget while tasks are still pending).
  void collect_deferred(neui_render_ctx_t ctx);
}
