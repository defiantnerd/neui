#pragma once
#include <neui/neui.h>

// HTML5 Canvas 2D rendering backend - implements neui_render_backend_t for
// the crossplatform host on the web (Emscripten). See plans/wasm-host.md.
//
// Every draw call goes straight to a CanvasRenderingContext2D through EM_JS
// (Stage 0 measured the JS <-> wasm crossings at ~15 % of a heavy frame, so
// there is no command buffer). The canvas keeps its pixels between frames,
// which is what makes partial repaint (set_frame_damage) cheap here.
//
// Window contexts: the platform layer creates the <canvas> element and
// registers it in the page as `Module.neuiCanvases[id]`; create_context's
// native_handle is that id cast through uintptr_t. width / height are the
// backing store's physical pixels, as on the other backends.

namespace neui_canvas2d_backend
{
  // Returns a pointer to the Canvas 2D backend's neui_render_backend_t.
  neui_render_backend_t* get_backend();

  // The exact device pixel ratio for a window context. update_dpi takes an
  // integer DPI, but the browser's ratio is fractional at most zoom levels
  // (96 x 1.1 is not a whole number); the platform passes the real value
  // through here after update_dpi. Takes effect at the next begin_frame.
  void set_scale(neui_render_ctx_t ctx, float scale);

  // The app leaves the page (unmount): removes the fonts it registered from
  // the document (font handles still held become no-ops). The backend stays
  // callable for the client's cleanup.
  void shutdown();
}
