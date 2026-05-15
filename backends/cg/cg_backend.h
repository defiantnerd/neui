#pragma once
#include <neui/neui.h>

// CoreGraphics rendering backend - implements neui_render_backend_t for the
// crossplatform host on macOS. Mirror of backends/d2d/d2d_backend.h.
//
// Usage contract: AppKit (NSView::drawRect:) hands us a fresh CGContextRef per
// frame. The platform layer calls neui_cg_backend::set_current_frame to bind
// that context and the view's logical size onto the per-window state right
// before invoking neui_render_backend_t::begin_frame. neui_render_backend_t::end_frame
// clears the binding so the same render context object can be reused next frame.

namespace neui_cg_backend
{
  // Returns a pointer to the CoreGraphics backend's neui_render_backend_t.
  neui_render_backend_t* get_backend();

  // Bind a CGContextRef + logical-pixel size onto a render context for the
  // duration of one frame. Call from drawRect: before begin_frame; the
  // backend forgets the binding at end_frame so it isn't held past the
  // CGContextRef's AppKit-managed lifetime.
  //
  // cg_context is `(void*)CGContextRef` to keep CoreGraphics out of clients.
  void set_current_frame(neui_render_ctx_t ctx, void* cg_context,
                         float width_logical, float height_logical);
}
