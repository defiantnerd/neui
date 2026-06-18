#pragma once

#if defined(__APPLE__)
#import <TargetConditionals.h>
#if TARGET_OS_IPHONE

#import <UIKit/UIKit.h>

#include <neui/neui.h>

#include <cstdint>

// iOS input-event translation primitives - the UIKit twin of keys_macos.h.
// Header-only / `inline` so the crossplatform iOS platform layer
// (hosts/crossplatform/platform_ios.mm) - and a future native iOS host - can
// both include without ODR violations.
//
// - ios_keycode_to_neui: UIKey.keyCode (a UIKeyboardHIDUsage USB HID usage
//   code from the keyboard/keypad page) -> neui_key_t (matches Win32 VK_*
//   values, same target set keys_macos.h covers - letters, digits, Tab, Return,
//   Esc, Backspace, Delete, arrows, Home/End/PageUp/Down, Space, F1-F12).
// - ios_modifiers_to_neui: UIKeyModifierFlags -> NEUI_KMOD_* bitmask. Per neui's
//   convention NEUI_KMOD_CTRL is the platform's *primary* command modifier, so
//   on iOS Command -> NEUI_KMOD_CTRL and the physical Control key -> NEUI_KMOD_META
//   (identical to macOS - see mac_modifiers_to_neui).
// - ios_is_printable_codepoint: gates which codepoints from UIKey.characters
//   should fire NEUI_EVENT_KEYCHAR (skips control chars and the UIKit/AppKit
//   0xF700..0xF8FF private-use function-key range, which UIKey.characters can
//   carry for arrows / nav keys).
//
// UIKeyboardHIDUsage values come from <UIKit/UIKey.h> (available iOS 13.4+);
// the raw HID usage IDs are stable (USB HID Usage Tables, Keyboard/Keypad page
// 0x07), so the constants below would be correct even on older SDKs, but the
// pressesBegan:/UIKey path that feeds them is 13.4+.

namespace neui_detail
{

// USB HID Keyboard/Keypad usage codes (page 0x07), matching the
// UIKeyboardHIDUsage* enumerators. Inlined as a local enum so the table reads
// like the kVK_* block in keys_macos.h and stays self-documenting; the values
// are the wire HID usages, identical to UIKeyboardHIDUsageKeyboard*.
enum : uint16_t {
  kHID_A = 0x04, kHID_B = 0x05, kHID_C = 0x06, kHID_D = 0x07,
  kHID_E = 0x08, kHID_F = 0x09, kHID_G = 0x0A, kHID_H = 0x0B,
  kHID_I = 0x0C, kHID_J = 0x0D, kHID_K = 0x0E, kHID_L = 0x0F,
  kHID_M = 0x10, kHID_N = 0x11, kHID_O = 0x12, kHID_P = 0x13,
  kHID_Q = 0x14, kHID_R = 0x15, kHID_S = 0x16, kHID_T = 0x17,
  kHID_U = 0x18, kHID_V = 0x19, kHID_W = 0x1A, kHID_X = 0x1B,
  kHID_Y = 0x1C, kHID_Z = 0x1D,
  // The number row: HID 1..9 then 0 (0x1E..0x27).
  kHID_1 = 0x1E, kHID_2 = 0x1F, kHID_3 = 0x20, kHID_4 = 0x21,
  kHID_5 = 0x22, kHID_6 = 0x23, kHID_7 = 0x24, kHID_8 = 0x25,
  kHID_9 = 0x26, kHID_0 = 0x27,
  kHID_Return    = 0x28,  // Return (Enter)
  kHID_Escape    = 0x29,
  kHID_Backspace = 0x2A,  // Delete (Backspace)
  kHID_Tab       = 0x2B,
  kHID_Space     = 0x2C,
  kHID_F1 = 0x3A, kHID_F2 = 0x3B, kHID_F3 = 0x3C, kHID_F4 = 0x3D,
  kHID_F5 = 0x3E, kHID_F6 = 0x3F, kHID_F7 = 0x40, kHID_F8 = 0x41,
  kHID_F9 = 0x42, kHID_F10 = 0x43, kHID_F11 = 0x44, kHID_F12 = 0x45,
  kHID_Insert      = 0x49,
  kHID_Home        = 0x4A,
  kHID_PageUp      = 0x4B,
  kHID_ForwardDel  = 0x4C,  // Delete Forward
  kHID_End         = 0x4D,
  kHID_PageDown    = 0x4E,
  kHID_RightArrow  = 0x4F,
  kHID_LeftArrow   = 0x50,
  kHID_DownArrow   = 0x51,
  kHID_UpArrow     = 0x52,
  kHID_KeypadEnter = 0x58,  // Keypad Enter -> Return
};

inline uint32_t ios_keycode_to_neui(uint16_t hid)
{
  switch (hid) {
    case kHID_A: return NEUI_KEY_A; case kHID_B: return NEUI_KEY_B;
    case kHID_C: return NEUI_KEY_C; case kHID_D: return NEUI_KEY_D;
    case kHID_E: return NEUI_KEY_E; case kHID_F: return NEUI_KEY_F;
    case kHID_G: return NEUI_KEY_G; case kHID_H: return NEUI_KEY_H;
    case kHID_I: return NEUI_KEY_I; case kHID_J: return NEUI_KEY_J;
    case kHID_K: return NEUI_KEY_K; case kHID_L: return NEUI_KEY_L;
    case kHID_M: return NEUI_KEY_M; case kHID_N: return NEUI_KEY_N;
    case kHID_O: return NEUI_KEY_O; case kHID_P: return NEUI_KEY_P;
    case kHID_Q: return NEUI_KEY_Q; case kHID_R: return NEUI_KEY_R;
    case kHID_S: return NEUI_KEY_S; case kHID_T: return NEUI_KEY_T;
    case kHID_U: return NEUI_KEY_U; case kHID_V: return NEUI_KEY_V;
    case kHID_W: return NEUI_KEY_W; case kHID_X: return NEUI_KEY_X;
    case kHID_Y: return NEUI_KEY_Y; case kHID_Z: return NEUI_KEY_Z;
    case kHID_0: return NEUI_KEY_0; case kHID_1: return NEUI_KEY_1;
    case kHID_2: return NEUI_KEY_2; case kHID_3: return NEUI_KEY_3;
    case kHID_4: return NEUI_KEY_4; case kHID_5: return NEUI_KEY_5;
    case kHID_6: return NEUI_KEY_6; case kHID_7: return NEUI_KEY_7;
    case kHID_8: return NEUI_KEY_8; case kHID_9: return NEUI_KEY_9;
    case kHID_F1:  return NEUI_KEY_F1;  case kHID_F2:  return NEUI_KEY_F2;
    case kHID_F3:  return NEUI_KEY_F3;  case kHID_F4:  return NEUI_KEY_F4;
    case kHID_F5:  return NEUI_KEY_F5;  case kHID_F6:  return NEUI_KEY_F6;
    case kHID_F7:  return NEUI_KEY_F7;  case kHID_F8:  return NEUI_KEY_F8;
    case kHID_F9:  return NEUI_KEY_F9;  case kHID_F10: return NEUI_KEY_F10;
    case kHID_F11: return NEUI_KEY_F11; case kHID_F12: return NEUI_KEY_F12;
    case kHID_Tab:         return NEUI_KEY_TAB;
    case kHID_Return:      return NEUI_KEY_RETURN;
    case kHID_KeypadEnter: return NEUI_KEY_RETURN;
    case kHID_Escape:      return NEUI_KEY_ESCAPE;
    case kHID_Space:       return NEUI_KEY_SPACE;
    case kHID_Backspace:   return NEUI_KEY_BACK;
    case kHID_ForwardDel:  return NEUI_KEY_DELETE;
    case kHID_Home:        return NEUI_KEY_HOME;
    case kHID_End:         return NEUI_KEY_END;
    case kHID_PageUp:      return NEUI_KEY_PAGEUP;
    case kHID_PageDown:    return NEUI_KEY_PAGEDOWN;
    case kHID_LeftArrow:   return NEUI_KEY_LEFT;
    case kHID_RightArrow:  return NEUI_KEY_RIGHT;
    case kHID_UpArrow:     return NEUI_KEY_UP;
    case kHID_DownArrow:   return NEUI_KEY_DOWN;
    case kHID_Insert:      return NEUI_KEY_INSERT;
    default:               return 0;
  }
}

inline uint32_t ios_modifiers_to_neui(UIKeyModifierFlags flags)
{
  uint32_t m = 0;
  // Mirror mac_modifiers_to_neui: Command is the platform-primary modifier
  // (-> NEUI_KMOD_CTRL), the physical Control key is the secondary
  // (-> NEUI_KMOD_META). This matches menu_ios_mods_to_uikit's inverse mapping.
  if (flags & UIKeyModifierShift)     m |= NEUI_KMOD_SHIFT;
  if (flags & UIKeyModifierCommand)   m |= NEUI_KMOD_CTRL;
  if (flags & UIKeyModifierAlternate) m |= NEUI_KMOD_ALT;
  if (flags & UIKeyModifierControl)   m |= NEUI_KMOD_META;
  return m;
}

// Inverse of menu_ios_key_to_input (menu_ios.h): a UIKeyCommand's `input`
// string -> the NEUI_KEY_* it was built from, so a key-command handler can
// recover the neui combo from the matched command (UIKeyCommand.propertyList is
// not settable via the public initializers, and the input/modifierFlags pair is
// the authoritative identity anyway). Returns NEUI_KEY_NONE for an unrecognised
// input. The mapping is the small set menu_ios_key_to_input emits: a..z, 0..9,
// the four arrows, and space.
inline uint32_t ios_input_to_neui_key(NSString* input)
{
  if (input.length == 0) return NEUI_KEY_NONE;
  if (input.length == 1) {
    unichar c = [input characterAtIndex:0];
    if (c >= 'a' && c <= 'z') return NEUI_KEY_A + (uint32_t)(c - 'a');
    if (c >= 'A' && c <= 'Z') return NEUI_KEY_A + (uint32_t)(c - 'A');
    if (c >= '0' && c <= '9') return NEUI_KEY_0 + (uint32_t)(c - '0');
    if (c == ' ')             return NEUI_KEY_SPACE;
  }
  if ([input isEqualToString:UIKeyInputLeftArrow])  return NEUI_KEY_LEFT;
  if ([input isEqualToString:UIKeyInputRightArrow]) return NEUI_KEY_RIGHT;
  if ([input isEqualToString:UIKeyInputUpArrow])    return NEUI_KEY_UP;
  if ([input isEqualToString:UIKeyInputDownArrow])  return NEUI_KEY_DOWN;
  return NEUI_KEY_NONE;
}

inline bool ios_is_printable_codepoint(uint32_t cp)
{
  if (cp < 0x20)                    return false;
  if (cp == 0x7F)                   return false;  // DEL
  if (cp >= 0xF700 && cp <= 0xF8FF) return false;  // UIKit private-use range
  return true;
}

} // namespace neui_detail

#endif // TARGET_OS_IPHONE
#endif // __APPLE__
