#pragma once
#include <neui/neui.h>

// Direct2D rendering backend - implements neui_render_backend_t using
// ID2D1HwndRenderTarget. Windows only.

namespace neui_d2d_backend
{
  // Returns a pointer to the D2D backend's neui_render_backend_t.
  neui_render_backend_t* get_backend();
}
