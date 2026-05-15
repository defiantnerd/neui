#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>

#include <neui/neui.h>

#include <cstdint>

// macOS input-event translation primitives. Header-only / `inline` so the
// crossplatform host (`hosts/crossplatform/platform_macos.mm`) and the native
// macOS host (`hosts/macos/`) can both include without ODR violations.
//
// - kVK_* virtual keycode constants (subset used by neui - letters, digits,
//   F1–F12, navigation / control). Inlined to avoid pulling in the
//   deprecated Carbon framework just for the values; constants come from
//   <Carbon/HIToolbox/Events.h>.
// - mac_keycode_to_neui: kVK_* → neui_key_t (matches Win32 VK_* values).
// - mac_modifiers_to_neui: NSEventModifierFlags → NEUI_KMOD_* bitmask. Per
//   neui's convention NEUI_KMOD_CTRL is the platform's *primary* command
//   modifier, so on macOS Cmd → NEUI_KMOD_CTRL and the physical Control key
//   → NEUI_KMOD_META.
// - mac_buttonmap: NSEvent.pressedMouseButtons + modifier flags → Win32
//   MK_* bitmask the host's mouse events carry in buttonmap.
// - is_printable_codepoint: gates which codepoints from
//   [NSEvent characters] should fire NEUI_EVENT_KEYCHAR (skips control
//   chars and AppKit's 0xF700..0xF8FF private-use range).

namespace neui_detail
{

enum : uint16_t {
  kVK_A = 0x00, kVK_S = 0x01, kVK_D = 0x02, kVK_F = 0x03, kVK_H = 0x04,
  kVK_G = 0x05, kVK_Z = 0x06, kVK_X = 0x07, kVK_C = 0x08, kVK_V = 0x09,
  kVK_B = 0x0B, kVK_Q = 0x0C, kVK_W = 0x0D, kVK_E = 0x0E, kVK_R = 0x0F,
  kVK_Y = 0x10, kVK_T = 0x11, kVK_1 = 0x12, kVK_2 = 0x13, kVK_3 = 0x14,
  kVK_4 = 0x15, kVK_6 = 0x16, kVK_5 = 0x17, kVK_9 = 0x19, kVK_7 = 0x1A,
  kVK_8 = 0x1C, kVK_0 = 0x1D, kVK_O = 0x1F, kVK_U = 0x20, kVK_I = 0x22,
  kVK_P = 0x23, kVK_Return = 0x24, kVK_L = 0x25, kVK_J = 0x26, kVK_K = 0x28,
  kVK_N = 0x2D, kVK_M = 0x2E, kVK_Tab = 0x30, kVK_Space = 0x31,
  kVK_Backspace = 0x33, kVK_Escape = 0x35,
  kVK_F5 = 0x60, kVK_F6 = 0x61, kVK_F7 = 0x62, kVK_F3 = 0x63, kVK_F8 = 0x64,
  kVK_F9 = 0x65, kVK_F11 = 0x67, kVK_F10 = 0x6D, kVK_F12 = 0x6F,
  kVK_Home = 0x73, kVK_PageUp = 0x74, kVK_ForwardDelete = 0x75,
  kVK_F4 = 0x76, kVK_End = 0x77, kVK_F2 = 0x78, kVK_PageDown = 0x79,
  kVK_F1 = 0x7A,
  kVK_LeftArrow = 0x7B, kVK_RightArrow = 0x7C,
  kVK_DownArrow = 0x7D, kVK_UpArrow = 0x7E,
  kVK_Insert = 0x72,  // "Help" on Mac kbd; nearest analogue.
};

inline uint32_t mac_keycode_to_neui(uint16_t mac_code)
{
  switch (mac_code) {
    case kVK_A: return NEUI_KEY_A; case kVK_B: return NEUI_KEY_B;
    case kVK_C: return NEUI_KEY_C; case kVK_D: return NEUI_KEY_D;
    case kVK_E: return NEUI_KEY_E; case kVK_F: return NEUI_KEY_F;
    case kVK_G: return NEUI_KEY_G; case kVK_H: return NEUI_KEY_H;
    case kVK_I: return NEUI_KEY_I; case kVK_J: return NEUI_KEY_J;
    case kVK_K: return NEUI_KEY_K; case kVK_L: return NEUI_KEY_L;
    case kVK_M: return NEUI_KEY_M; case kVK_N: return NEUI_KEY_N;
    case kVK_O: return NEUI_KEY_O; case kVK_P: return NEUI_KEY_P;
    case kVK_Q: return NEUI_KEY_Q; case kVK_R: return NEUI_KEY_R;
    case kVK_S: return NEUI_KEY_S; case kVK_T: return NEUI_KEY_T;
    case kVK_U: return NEUI_KEY_U; case kVK_V: return NEUI_KEY_V;
    case kVK_W: return NEUI_KEY_W; case kVK_X: return NEUI_KEY_X;
    case kVK_Y: return NEUI_KEY_Y; case kVK_Z: return NEUI_KEY_Z;
    case kVK_0: return NEUI_KEY_0; case kVK_1: return NEUI_KEY_1;
    case kVK_2: return NEUI_KEY_2; case kVK_3: return NEUI_KEY_3;
    case kVK_4: return NEUI_KEY_4; case kVK_5: return NEUI_KEY_5;
    case kVK_6: return NEUI_KEY_6; case kVK_7: return NEUI_KEY_7;
    case kVK_8: return NEUI_KEY_8; case kVK_9: return NEUI_KEY_9;
    case kVK_F1:  return NEUI_KEY_F1;  case kVK_F2:  return NEUI_KEY_F2;
    case kVK_F3:  return NEUI_KEY_F3;  case kVK_F4:  return NEUI_KEY_F4;
    case kVK_F5:  return NEUI_KEY_F5;  case kVK_F6:  return NEUI_KEY_F6;
    case kVK_F7:  return NEUI_KEY_F7;  case kVK_F8:  return NEUI_KEY_F8;
    case kVK_F9:  return NEUI_KEY_F9;  case kVK_F10: return NEUI_KEY_F10;
    case kVK_F11: return NEUI_KEY_F11; case kVK_F12: return NEUI_KEY_F12;
    case kVK_Tab:           return NEUI_KEY_TAB;
    case kVK_Return:        return NEUI_KEY_RETURN;
    case kVK_Escape:        return NEUI_KEY_ESCAPE;
    case kVK_Space:         return NEUI_KEY_SPACE;
    case kVK_Backspace:     return NEUI_KEY_BACK;
    case kVK_ForwardDelete: return NEUI_KEY_DELETE;
    case kVK_Home:          return NEUI_KEY_HOME;
    case kVK_End:           return NEUI_KEY_END;
    case kVK_LeftArrow:     return NEUI_KEY_LEFT;
    case kVK_RightArrow:    return NEUI_KEY_RIGHT;
    case kVK_UpArrow:       return NEUI_KEY_UP;
    case kVK_DownArrow:     return NEUI_KEY_DOWN;
    case kVK_Insert:        return NEUI_KEY_INSERT;
    default:                return 0;
  }
}

inline uint32_t mac_modifiers_to_neui(NSEventModifierFlags flags)
{
  uint32_t m = 0;
  if (flags & NSEventModifierFlagShift)   m |= NEUI_KMOD_SHIFT;
  if (flags & NSEventModifierFlagCommand) m |= NEUI_KMOD_CTRL;
  if (flags & NSEventModifierFlagOption)  m |= NEUI_KMOD_ALT;
  if (flags & NSEventModifierFlagControl) m |= NEUI_KMOD_META;
  return m;
}

inline uint32_t mac_buttonmap(NSUInteger pressed_buttons,
                               NSEventModifierFlags flags)
{
  uint32_t m = 0;
  if (pressed_buttons & 0x1) m |= 0x0001;                 // MK_LBUTTON
  if (pressed_buttons & 0x2) m |= 0x0002;                 // MK_RBUTTON
  if (flags & NSEventModifierFlagShift)   m |= 0x0004;     // MK_SHIFT
  if (flags & NSEventModifierFlagControl) m |= 0x0008;     // MK_CONTROL
  return m;
}

inline bool is_printable_codepoint(uint32_t cp)
{
  if (cp < 0x20)                    return false;
  if (cp == 0x7F)                   return false;  // DEL
  if (cp >= 0xF700 && cp <= 0xF8FF) return false;  // AppKit private-use range
  return true;
}

} // namespace neui_detail

#endif // __APPLE__
