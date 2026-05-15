#pragma once
#include <neui/neui.h>

// Null rendering backend - no-op implementation of neui_render_backend_t.
// Used on platforms where no real rendering backend is available yet.
// All operations succeed silently; no pixels are produced.

namespace neui_null_backend
{
  // Returns a pointer to the null backend's neui_render_backend_t.
  neui_render_backend_t* get_backend();
}
