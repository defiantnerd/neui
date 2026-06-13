#pragma once

#if defined(__linux__) || (defined(__unix__) && !defined(__APPLE__))

#include <neui/neui.h>

#include <X11/Xlib.h>
#define XK_LATIN1
#define XK_MISCELLANY
#define XK_XKB_KEYS          // XK_ISO_Left_Tab (Shift+Tab)
#include <X11/keysymdef.h>

#include <cstdint>

// X11 input-event translation primitives for the crossplatform host's Linux
// platform layer. Mirror of hosts/shared/macos/keys_macos.h:
//   - x11_keysym_to_neui:  KeySym -> neui_key_t (matches Win32 VK_* values).
//   - x11_modifiers_to_neui: XKeyEvent.state -> NEUI_KMOD_* bitmask. Per neui's
//     convention NEUI_KMOD_CTRL is the platform PRIMARY modifier, which on
//     Linux is the physical Control key; Super (Mod4) -> NEUI_KMOD_META.
//   - x11_buttonmap: button/motion event state -> Win32 MK_* bitmask carried
//     in neui_event_mouse_t::buttonmap.
//   - is_printable_codepoint: gates which decoded codepoints fire KEYCHAR.

namespace neui_detail
{

inline uint32_t x11_keysym_to_neui(KeySym ks)
{
  // Letters: X gives lower- or upper-case depending on Shift/Lock; neui keys
  // are the uppercase ASCII value (== Win32 VK_A..VK_Z), so fold to upper.
  if (ks >= XK_a && ks <= XK_z) return NEUI_KEY_A + (uint32_t)(ks - XK_a);
  if (ks >= XK_A && ks <= XK_Z) return NEUI_KEY_A + (uint32_t)(ks - XK_A);
  if (ks >= XK_0 && ks <= XK_9) return NEUI_KEY_0 + (uint32_t)(ks - XK_0);
  if (ks >= XK_F1 && ks <= XK_F12) return NEUI_KEY_F1 + (uint32_t)(ks - XK_F1);

  switch (ks) {
    case XK_BackSpace: return NEUI_KEY_BACK;
    case XK_Tab:       return NEUI_KEY_TAB;
    case XK_ISO_Left_Tab: return NEUI_KEY_TAB;   // Shift+Tab on X
    case XK_Return:
    case XK_KP_Enter:  return NEUI_KEY_RETURN;
    case XK_Escape:    return NEUI_KEY_ESCAPE;
    case XK_space:     return NEUI_KEY_SPACE;
    case XK_Prior:     return NEUI_KEY_PAGEUP;
    case XK_Next:      return NEUI_KEY_PAGEDOWN;
    case XK_End:       return NEUI_KEY_END;
    case XK_Home:      return NEUI_KEY_HOME;
    case XK_Left:      return NEUI_KEY_LEFT;
    case XK_Up:        return NEUI_KEY_UP;
    case XK_Right:     return NEUI_KEY_RIGHT;
    case XK_Down:      return NEUI_KEY_DOWN;
    case XK_Insert:    return NEUI_KEY_INSERT;
    case XK_Delete:    return NEUI_KEY_DELETE;
    case XK_KP_Delete: return NEUI_KEY_DELETE;
    default:           return 0;
  }
}

inline uint32_t x11_modifiers_to_neui(unsigned int state)
{
  uint32_t m = 0;
  if (state & ShiftMask)   m |= NEUI_KMOD_SHIFT;
  if (state & ControlMask) m |= NEUI_KMOD_CTRL;   // primary modifier on Linux
  if (state & Mod1Mask)    m |= NEUI_KMOD_ALT;     // Alt
  if (state & Mod4Mask)    m |= NEUI_KMOD_META;    // Super / Win key
  return m;
}

// Win32 MK_* bits the host's mouse events carry in buttonmap. The numeric
// values match Win32 MK_LBUTTON / MK_RBUTTON / MK_SHIFT / MK_CONTROL.
inline uint32_t x11_buttonmap(unsigned int state)
{
  uint32_t m = 0;
  if (state & Button1Mask) m |= 0x0001;  // MK_LBUTTON
  if (state & Button3Mask) m |= 0x0002;  // MK_RBUTTON
  if (state & Button2Mask) m |= 0x0010;  // MK_MBUTTON
  if (state & ShiftMask)   m |= 0x0004;  // MK_SHIFT
  if (state & ControlMask) m |= 0x0008;  // MK_CONTROL
  return m;
}

inline bool is_printable_codepoint(uint32_t cp)
{
  if (cp < 0x20)  return false;
  if (cp == 0x7F) return false;  // DEL
  return true;
}

} // namespace neui_detail

#endif // linux
