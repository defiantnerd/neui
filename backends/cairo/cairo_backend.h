#pragma once

#include <neui/neui.h>        // stdint + NEUI_VERSION (must precede d/ headers)
#include <neui/d/renderer.h>

#include <X11/Xlib.h>

// Cairo software rendering backend for the crossplatform host on Linux.
//
// Shape mirror of backends/cg/cg_backend.mm:
//   - One cairo_t per context (created at create_context, lives for the
//     ctx's lifetime - NOT rebound per frame, so no set_current_frame seam).
//   - Path API -> cairo_new_path / move_to / line_to / arc, fill/stroke.
//   - Transform + clip stack -> cairo_save / cairo_restore.
//   - Bitmap -> cairo image surface; text -> Fontconfig + cairo FreeType.
//
// Cairo is top-left, Y-down natively, so unlike the CG backend there is no
// Y-flip anywhere.
namespace neui_cairo_backend
{
  // Passed by the platform layer as the `native_handle` to create_context.
  // The backend copies these fields into its per-context state; the struct
  // itself only needs to outlive the create_context call (the Display
  // connection + Window are owned by the platform layer, borrowed here).
  struct LinuxNativeSurface
  {
    Display* dpy    = nullptr;
    Window   win    = 0;
    Visual*  visual = nullptr;
    int      depth  = 0;
  };

  neui_render_backend_t* get_backend();
}
