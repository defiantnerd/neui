#pragma once

#ifdef _WIN32

#include <neui/neui.h>

#include <windows.h>

#include <cstdint>

// Win32 input-modifier translation primitives shared by BOTH win32 hosts (the
// native HWND host and the crossplatform host's win32 platform layer).
// Completes the set alongside hosts/shared/macos/keys_macos.h and
// hosts/shared/linux/keys_linux.h:
//   - win32_buttonmap:            WPARAM key state -> NEUI_MK_* bitmask.
//   - win32_buttonmap_from_state: live key state   -> NEUI_MK_* bitmask.
//   - win32_kmod_from_state:      live key state   -> NEUI_KMOD_* bitmask.
//
// Prefer the WPARAM form wherever the message carries key state (WM_MOUSEMOVE,
// WM_?BUTTON*, and - via GET_KEYSTATE_WPARAM - the wheel messages): that is
// the state as of when the message was POSTED, whereas GetKeyState reflects
// now, which can differ on a backed-up queue. The *_from_state forms exist for
// the messages that carry none (WM_MOUSELEAVE, WM_KEYDOWN / WM_KEYUP).

namespace neui_detail
{

// NEUI_MK_* numerically match the Win32 MK_* constants (see the note on the
// enum in <neui/d/events.h>), so a wParam key state forwards unmodified. This
// wrapper exists so call sites name the contract instead of relying on the
// coincidence silently.
inline uint32_t win32_buttonmap(WPARAM key_state)
{
  return static_cast<uint32_t>(key_state)
       & (NEUI_MK_LBUTTON | NEUI_MK_RBUTTON | NEUI_MK_MBUTTON
          | NEUI_MK_SHIFT | NEUI_MK_CONTROL);
}

inline uint32_t win32_buttonmap_from_state()
{
  uint32_t m = 0;
  if (GetKeyState(VK_LBUTTON) & 0x8000) m |= NEUI_MK_LBUTTON;
  if (GetKeyState(VK_RBUTTON) & 0x8000) m |= NEUI_MK_RBUTTON;
  if (GetKeyState(VK_MBUTTON) & 0x8000) m |= NEUI_MK_MBUTTON;
  if (GetKeyState(VK_SHIFT)   & 0x8000) m |= NEUI_MK_SHIFT;
  if (GetKeyState(VK_CONTROL) & 0x8000) m |= NEUI_MK_CONTROL;
  return m;
}

// NEUI_KMOD_CTRL is neui's PRIMARY modifier, which on Windows is the physical
// Control key; NEUI_KMOD_META is the secondary (Win / Super). VK_LWIN / VK_RWIN
// do not report through GetKeyState reliably while the shell owns the key, so
// META is best-effort - accelerators go through the HACCEL table, not here.
inline uint32_t win32_kmod_from_state()
{
  uint32_t m = 0;
  if (GetKeyState(VK_SHIFT)   & 0x8000) m |= NEUI_KMOD_SHIFT;
  if (GetKeyState(VK_CONTROL) & 0x8000) m |= NEUI_KMOD_CTRL;
  if (GetKeyState(VK_MENU)    & 0x8000) m |= NEUI_KMOD_ALT;
  if ((GetKeyState(VK_LWIN) & 0x8000) || (GetKeyState(VK_RWIN) & 0x8000))
    m |= NEUI_KMOD_META;
  return m;
}

} // namespace neui_detail

#endif // _WIN32
