#pragma once
#include <neui/neui.h>

// Direct2D rendering backend - implements neui_render_backend_t with D2D 1.1
// device contexts on a process-wide D3D11 device: DXGI flip swap chains for
// windows, GPU target bitmaps for surfaces, and custom effects for GPU
// shader programs (d2d_shader.cpp). Windows only.

namespace neui_d2d_backend
{
  // Returns a pointer to the D2D backend's neui_render_backend_t.
  neui_render_backend_t* get_backend();
}
