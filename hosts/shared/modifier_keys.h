#pragma once

#include <cstdint>

#include <neui/d/keys.h>

#include "mouse_modifiers.h"

// The modifier keys AS KEYS: NEUI_KEY_SHIFT / _CONTROL / _ALT / _COMMAND
// (<neui/d/keys.h>), delivered as NEUI_EVENT_KEYDOWN / NEUI_EVENT_KEYUP when a
// modifier is pressed or released on its own.
//
// Like mouse_modifiers.h this is the portable, Tier-1 tested core of three
// platform translations, written over plain integers so the one machine that
// builds the suite covers all of them. The OS-header-specific wrappers live in
// hosts/shared/{win32/keys_win32.h,macos/keys_macos.h,linux/keys_linux.h}, which
// static_assert that the raw constants below are the SDKs' own.
//
// THE CONTRACT every platform layer implements with these:
//   - one KEYDOWN when a modifier goes down, one KEYUP when it comes up; a held
//     modifier does NOT repeat (Win32's typematic WM_KEYDOWN is filtered out);
//   - the left and right key of a kind share one code;
//   - `modifiers` is the NEUI_KMOD_* state AFTER the change, so the KEYDOWN of
//     Shift already carries NEUI_KMOD_SHIFT and its KEYUP no longer does;
//   - never a KEYCHAR - a modifier produces no text.

namespace neui_detail
{
  // ---- Win32 ---------------------------------------------------------------

  // VK_* of the modifier keys. The generic ones (SHIFT / CONTROL / MENU) are what
  // WM_KEYDOWN reports; the left / right variants are folded in as well so a
  // synthesized or driver-specific message cannot slip past as an unknown key.
  // Returns 0 for any other virtual key.
  inline uint32_t vk_to_modifier_key(uint32_t vk)
  {
    switch (vk) {
      case 0x10: case 0xA0: case 0xA1: return NEUI_KEY_SHIFT;     // VK_SHIFT, VK_LSHIFT, VK_RSHIFT
      case 0x11: case 0xA2: case 0xA3: return NEUI_KEY_CONTROL;   // VK_CONTROL, VK_LCONTROL, VK_RCONTROL
      case 0x12: case 0xA4: case 0xA5: return NEUI_KEY_ALT;       // VK_MENU, VK_LMENU, VK_RMENU
      case 0x5B: case 0x5C:            return NEUI_KEY_COMMAND;   // VK_LWIN, VK_RWIN
      default:                         return 0;
    }
  }

  // ---- X11 -----------------------------------------------------------------

  // KeySym values (<X11/keysymdef.h>); keys_linux.h static_asserts them.
  namespace x11_keysym
  {
    constexpr unsigned long kShiftL   = 0xffe1;
    constexpr unsigned long kShiftR   = 0xffe2;
    constexpr unsigned long kControlL = 0xffe3;
    constexpr unsigned long kControlR = 0xffe4;
    constexpr unsigned long kMetaL    = 0xffe7;
    constexpr unsigned long kMetaR    = 0xffe8;
    constexpr unsigned long kAltL     = 0xffe9;
    constexpr unsigned long kAltR     = 0xffea;
    constexpr unsigned long kSuperL   = 0xffeb;
    constexpr unsigned long kSuperR   = 0xffec;
  }

  // The modifier keysyms. Alt and Meta are one kind here (both are Mod1, which is
  // what NEUI_MK_ALT / NEUI_KMOD_ALT read). Lock keys, AltGr (ISO_Level3_Shift)
  // and Hyper are not modifiers neui reports. Returns 0 for any other keysym.
  inline uint32_t x11_keysym_to_modifier_key(unsigned long ks)
  {
    switch (ks) {
      case x11_keysym::kShiftL:   case x11_keysym::kShiftR:   return NEUI_KEY_SHIFT;
      case x11_keysym::kControlL: case x11_keysym::kControlR: return NEUI_KEY_CONTROL;
      case x11_keysym::kAltL:     case x11_keysym::kAltR:
      case x11_keysym::kMetaL:    case x11_keysym::kMetaR:    return NEUI_KEY_ALT;
      case x11_keysym::kSuperL:   case x11_keysym::kSuperR:   return NEUI_KEY_COMMAND;
      default:                                                return 0;
    }
  }

  // ---- macOS ---------------------------------------------------------------

  // One modifier that changed between two flagsChanged: events.
  struct ModifierKeyChange
  {
    uint32_t keycode;   // NEUI_KEY_SHIFT / _CONTROL / _ALT / _COMMAND
    bool     down;      // true = pressed, false = released
  };

  // The modifier keys whose state differs between two NSEventModifierFlags
  // values, in a fixed order (Shift, Control, Option, Command). AppKit reports a
  // modifier as one flag however many physical keys of that kind are down, so
  // releasing the left Shift while the right one is still held changes nothing
  // and yields no entry. Caps Lock, Fn and the device-dependent bits are not in
  // the four flags this looks at. Returns how many entries of `out` were filled.
  inline int mac_modifier_key_changes(uint64_t prev_flags, uint64_t next_flags,
                                      ModifierKeyChange out[4])
  {
    struct Row { uint64_t flag; uint32_t key; };
    static const Row rows[4] = {
      { ns_input::kShift,   NEUI_KEY_SHIFT   },
      { ns_input::kControl, NEUI_KEY_CONTROL },
      { ns_input::kOption,  NEUI_KEY_ALT     },
      { ns_input::kCommand, NEUI_KEY_COMMAND },
    };
    int n = 0;
    for (const Row& r : rows) {
      const bool was = (prev_flags & r.flag) != 0;
      const bool is  = (next_flags & r.flag) != 0;
      if (was != is) out[n++] = { r.key, is };
    }
    return n;
  }

  // ---- NEUI_KMOD_* state after a modifier key changed ------------------------

  // The NEUI_KMOD_* bit a modifier KEY sets. KMOD_CTRL is the platform's PRIMARY
  // modifier, so the physical Control key and the Command / Windows / Super key
  // trade places between macOS and the other two - the table next to NEUI_MK_*
  // in <neui/d/events.h>. 0 for a key that is not a modifier.
  inline uint32_t kmod_bit_for_modifier_key(uint32_t neui_key, bool mac)
  {
    switch (neui_key) {
      case NEUI_KEY_SHIFT:   return NEUI_KMOD_SHIFT;
      case NEUI_KEY_ALT:     return NEUI_KMOD_ALT;
      case NEUI_KEY_CONTROL: return mac ? NEUI_KMOD_META : NEUI_KMOD_CTRL;
      case NEUI_KEY_COMMAND: return mac ? NEUI_KMOD_CTRL : NEUI_KMOD_META;
      default:               return 0;
    }
  }

  // `kmod` is the modifier state an OS reports WITH a key event. X11 reports the
  // state from BEFORE the event (a Shift press arrives with the Shift bit still
  // clear; its release with the bit still set), whereas the contract above wants
  // the state after. Apply the change of the key itself.
  inline uint32_t kmod_after_modifier_key(uint32_t kmod, uint32_t neui_key,
                                          bool down, bool mac)
  {
    const uint32_t bit = kmod_bit_for_modifier_key(neui_key, mac);
    return down ? (kmod | bit) : (kmod & ~bit);
  }
}
