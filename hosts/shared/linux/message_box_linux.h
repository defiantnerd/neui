#pragma once

#if defined(__linux__) || (defined(__unix__) && !defined(__APPLE__))

// Pure flag->buttons/icon parsing for the Linux message box. X11 has no
// native dialog, so platform_linux.cpp draws the box itself with the Cairo
// backend + a nested modal loop; this header only owns the MessageBoxEx-shaped
// NEUI_MB_* decode (button set, default button, Esc/cancel button, icon
// class), in Win32 visual order with the matching NEUI_ID_* returns.

#include <neui/d/notify.h>

#include <cstdint>

namespace neui_detail
{
  struct MsgBoxButton { const char* label; int id; };

  struct MsgBoxSpec
  {
    MsgBoxButton btn[3]   = {};
    int          count    = 0;
    int          def_index    = 0;   // Enter activates this button
    int          cancel_index = -1;  // Esc / close activates this button (-1 = none)
    uint32_t     icon     = 0;       // NEUI_MB_ICON* nibble (0x10/0x20/0x30/0x40) or 0
  };

  inline MsgBoxSpec msgbox_parse(uint32_t flags)
  {
    MsgBoxSpec s;
    s.icon = flags & 0x00F0u;
    auto set = [&](int i, const char* l, int id) { s.btn[i].label = l; s.btn[i].id = id; };
    switch (flags & 0x000Fu) {
      default:
      case NEUI_MB_OK:
        set(0, "OK", NEUI_ID_OK); s.count = 1; break;
      case NEUI_MB_OKCANCEL:
        set(0, "OK", NEUI_ID_OK); set(1, "Cancel", NEUI_ID_CANCEL); s.count = 2; break;
      case NEUI_MB_ABORTRETRYIGNORE:
        set(0, "Abort", NEUI_ID_ABORT); set(1, "Retry", NEUI_ID_RETRY);
        set(2, "Ignore", NEUI_ID_IGNORE); s.count = 3; break;
      case NEUI_MB_YESNOCANCEL:
        set(0, "Yes", NEUI_ID_YES); set(1, "No", NEUI_ID_NO);
        set(2, "Cancel", NEUI_ID_CANCEL); s.count = 3; break;
      case NEUI_MB_YESNO:
        set(0, "Yes", NEUI_ID_YES); set(1, "No", NEUI_ID_NO); s.count = 2; break;
      case NEUI_MB_RETRYCANCEL:
        set(0, "Retry", NEUI_ID_RETRY); set(1, "Cancel", NEUI_ID_CANCEL); s.count = 2; break;
      case NEUI_MB_CANCELTRYCONTINUE:
        set(0, "Cancel", NEUI_ID_CANCEL); set(1, "Try Again", NEUI_ID_TRYAGAIN);
        set(2, "Continue", NEUI_ID_CONTINUE); s.count = 3; break;
    }
    int def = (int)((flags & 0x0F00u) >> 8);
    if (def >= 0 && def < s.count) s.def_index = def;
    // Esc maps to Cancel if present, else to the sole OK button, else nothing.
    for (int i = 0; i < s.count; ++i)
      if (s.btn[i].id == NEUI_ID_CANCEL) s.cancel_index = i;
    if (s.cancel_index < 0 && s.count == 1) s.cancel_index = 0;
    return s;
  }

} // namespace neui_detail

#endif // linux
