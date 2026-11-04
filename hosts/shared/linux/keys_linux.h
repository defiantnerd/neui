#pragma once

#if defined(__linux__) || (defined(__unix__) && !defined(__APPLE__))

#include <neui/neui.h>

#include <X11/Xlib.h>
#define XK_LATIN1
#define XK_MISCELLANY
#define XK_XKB_KEYS          // XK_ISO_Left_Tab (Shift+Tab)
#include <X11/keysymdef.h>

#include <cstdint>

#include "../mouse_modifiers.h"
#include "../modifier_keys.h"

// X11 input-event translation primitives for the crossplatform host's Linux
// platform layer. Mirror of hosts/shared/macos/keys_macos.h:
//   - x11_keysym_to_neui:  KeySym -> neui_key_t (matches Win32 VK_* values).
//   - x11_modifiers_to_neui: XKeyEvent.state -> NEUI_KMOD_* bitmask. Per neui's
//     convention NEUI_KMOD_CTRL is the platform PRIMARY modifier, which on
//     Linux is the physical Control key; Super (Mod4) -> NEUI_KMOD_META.
//   - x11_buttonmap: button/motion event state -> NEUI_MK_* bitmask carried
//     in neui_event_mouse_t::buttonmap (Mod1 -> NEUI_MK_ALT, Mod4 ->
//     NEUI_MK_COMMAND). The decoding is the portable
//     hosts/shared/mouse_modifiers.h::mk_from_x11.
//   - x11_modifiers_after_key: the modifier keys as KEYS (Shift_L .. Super_R ->
//     NEUI_KEY_SHIFT .. _COMMAND, via x11_keysym_to_neui) and the NEUI_KMOD_*
//     state AFTER one of them changed - X11 reports the state from before.
//   - is_printable_codepoint: gates which decoded codepoints fire KEYCHAR.

namespace neui_detail
{

// The raw values hosts/shared/mouse_modifiers.h and modifier_keys.h decode (so
// that they need no X11 headers) are Xlib's own. Checked here, where Xlib is on
// hand.
static_assert(static_cast<unsigned int>(ShiftMask)   == x11_state::kShift
           && static_cast<unsigned int>(ControlMask) == x11_state::kControl
           && static_cast<unsigned int>(Mod1Mask)    == x11_state::kMod1
           && static_cast<unsigned int>(Mod4Mask)    == x11_state::kMod4
           && static_cast<unsigned int>(Button1Mask) == x11_state::kButton1
           && static_cast<unsigned int>(Button2Mask) == x11_state::kButton2
           && static_cast<unsigned int>(Button3Mask) == x11_state::kButton3,
              "x11_state values no longer match <X11/X.h>");
static_assert(static_cast<unsigned long>(XK_Shift_L)   == x11_keysym::kShiftL
           && static_cast<unsigned long>(XK_Shift_R)   == x11_keysym::kShiftR
           && static_cast<unsigned long>(XK_Control_L) == x11_keysym::kControlL
           && static_cast<unsigned long>(XK_Control_R) == x11_keysym::kControlR
           && static_cast<unsigned long>(XK_Meta_L)    == x11_keysym::kMetaL
           && static_cast<unsigned long>(XK_Meta_R)    == x11_keysym::kMetaR
           && static_cast<unsigned long>(XK_Alt_L)     == x11_keysym::kAltL
           && static_cast<unsigned long>(XK_Alt_R)     == x11_keysym::kAltR
           && static_cast<unsigned long>(XK_Super_L)   == x11_keysym::kSuperL
           && static_cast<unsigned long>(XK_Super_R)   == x11_keysym::kSuperR,
              "x11_keysym values no longer match <X11/keysymdef.h>");

inline uint32_t x11_keysym_to_neui(KeySym ks)
{
  // The modifier keys, as keys. Either side maps to one code; Alt and Meta are
  // both Mod1 (see modifier_keys.h).
  if (const uint32_t mod = x11_keysym_to_modifier_key(ks)) return mod;

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

// NEUI_MK_* bits the host's mouse events carry in buttonmap.
inline uint32_t x11_buttonmap(unsigned int state)
{
  return mk_from_x11(state);
}

// The NEUI_KMOD_* state a KeyPress / KeyRelease of the modifier key `keycode`
// (an NEUI_KEY_SHIFT / _CONTROL / _ALT / _COMMAND from x11_keysym_to_neui)
// leaves behind. XKeyEvent.state is the state from BEFORE the event: a Shift
// press arrives with the Shift bit clear, its release with the bit still set.
inline uint32_t x11_modifiers_after_key(unsigned int state, uint32_t keycode,
                                        bool down)
{
  return kmod_after_modifier_key(x11_modifiers_to_neui(state), keycode, down,
                                 /*mac=*/false);
}

inline bool is_printable_codepoint(uint32_t cp)
{
  if (cp < 0x20)  return false;
  if (cp == 0x7F) return false;  // DEL
  return true;
}

} // namespace neui_detail

#endif // linux
