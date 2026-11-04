#pragma once

#include <cstdint>

// events.h is not self-sufficient (its payloads use neui_item_t from api.h), so
// include that first rather than relying on the includer's order.
#include <neui/d/api.h>
#include <neui/d/events.h>

// The portable core of the three per-OS buttonmap translations.
//
// A mouse / wheel / DnD event carries the mouse buttons held and the modifier
// KEYS held as NEUI_MK_* bits (<neui/d/events.h>). Each OS reports that state in
// its own shape:
//
//   Win32  the MK_* low word of a mouse message's wParam - which has NO Alt bit
//          and NO Windows-key bit, so those two come from GetKeyState; and
//          OLE's grfKeyState during a drag, whose 0x20 bit IS Alt.
//   X11    XButtonEvent.state: ShiftMask / ControlMask / Mod1Mask (Alt) /
//          Mod4Mask (Super) / Button1Mask ...
//   macOS  NSEvent.pressedMouseButtons + NSEventModifierFlags.
//
// The decoding is plain bit arithmetic over integers, so it lives HERE - in a
// header that includes no OS header - and is Tier-1 tested
// (tests/test_mouse_modifiers.cpp), instead of inside three platform-guarded
// headers of which this machine can compile one. hosts/shared/{win32/keys_win32.h,
// macos/keys_macos.h,linux/keys_linux.h} are the thin wrappers that read the
// live OS state and call these; each also static_asserts, where the SDK header
// is on hand, that the raw constants below are the SDK's own.
//
// Platform layers must go through those wrappers (win32_buttonmap /
// mac_buttonmap / x11_buttonmap) and never forward a raw OS state word: the
// numeric values of the first five NEUI_MK_* were chosen to match Win32's MK_*,
// but MK_XBUTTON1 is 0x0020 - the bit NEUI_MK_ALT uses.

namespace neui_detail
{
  // Win32 mouse-message key state (the wParam low word; for the wheel messages,
  // GET_KEYSTATE_WPARAM). The five bits that are the same thing in both
  // vocabularies are kept; everything else is dropped on purpose - above all
  // 0x0020 (MK_XBUTTON1, the first side button) and 0x0040 (MK_XBUTTON2), which
  // would otherwise read as Alt and as nothing-yet-defined.
  //
  // `alt` / `command` are the live state of VK_MENU and VK_LWIN / VK_RWIN: the
  // mouse messages carry neither.
  inline uint32_t mk_from_win32(uint32_t key_state, bool alt, bool command)
  {
    uint32_t m = key_state & (NEUI_MK_LBUTTON | NEUI_MK_RBUTTON | NEUI_MK_MBUTTON
                              | NEUI_MK_SHIFT | NEUI_MK_CONTROL);
    if (alt)     m |= NEUI_MK_ALT;
    if (command) m |= NEUI_MK_COMMAND;
    return m;
  }

  // OLE drag-and-drop key state (IDropTarget's grfKeyState). Same bits as the
  // mouse messages with one difference that matters: OLE defines 0x0020 as
  // MK_ALT, not as the side button, so here that bit IS Alt and is kept. The
  // Windows key is not part of grfKeyState; `command` is read live.
  inline uint32_t mk_from_win32_ole(uint32_t grf_key_state, bool command)
  {
    uint32_t m = grf_key_state & (NEUI_MK_LBUTTON | NEUI_MK_RBUTTON | NEUI_MK_MBUTTON
                                  | NEUI_MK_SHIFT | NEUI_MK_CONTROL | NEUI_MK_ALT);
    if (command) m |= NEUI_MK_COMMAND;
    return m;
  }

  // X11 core event state bits. These are fixed by the X protocol; keys_linux.h
  // static_asserts them against <X11/X.h>.
  namespace x11_state
  {
    constexpr unsigned int kShift   = 1u << 0;   // ShiftMask
    constexpr unsigned int kControl = 1u << 2;   // ControlMask
    constexpr unsigned int kMod1    = 1u << 3;   // Mod1Mask   - Alt on every common keymap
    constexpr unsigned int kMod4    = 1u << 6;   // Mod4Mask   - Super / the Windows key
    constexpr unsigned int kButton1 = 1u << 8;   // Button1Mask
    constexpr unsigned int kButton2 = 1u << 9;   // Button2Mask
    constexpr unsigned int kButton3 = 1u << 10;  // Button3Mask
  }

  // XButtonEvent / XMotionEvent / XIModifierState::effective -> NEUI_MK_*.
  // Lock (Caps) and Mod2 (NumLock) are deliberately ignored.
  inline uint32_t mk_from_x11(unsigned int state)
  {
    uint32_t m = 0;
    if (state & x11_state::kButton1) m |= NEUI_MK_LBUTTON;
    if (state & x11_state::kButton3) m |= NEUI_MK_RBUTTON;   // X button 3 is the right one
    if (state & x11_state::kButton2) m |= NEUI_MK_MBUTTON;
    if (state & x11_state::kShift)   m |= NEUI_MK_SHIFT;
    if (state & x11_state::kControl) m |= NEUI_MK_CONTROL;
    if (state & x11_state::kMod1)    m |= NEUI_MK_ALT;
    if (state & x11_state::kMod4)    m |= NEUI_MK_COMMAND;
    return m;
  }

  // AppKit's NSEventModifierFlags and NSEvent.pressedMouseButtons bits.
  // keys_macos.h static_asserts the flags against AppKit's own constants.
  namespace ns_input
  {
    constexpr uint64_t kShift   = 1ull << 17;    // NSEventModifierFlagShift
    constexpr uint64_t kControl = 1ull << 18;    // NSEventModifierFlagControl
    constexpr uint64_t kOption  = 1ull << 19;    // NSEventModifierFlagOption
    constexpr uint64_t kCommand = 1ull << 20;    // NSEventModifierFlagCommand
    constexpr uint64_t kLeft    = 1ull << 0;     // pressedMouseButtons bit 0
    constexpr uint64_t kRight   = 1ull << 1;     // pressedMouseButtons bit 1
  }

  // NSEvent.pressedMouseButtons + NSEventModifierFlags -> NEUI_MK_*. Control is
  // the physical Control key, not the Cmd key that NEUI_KMOD_CTRL stands for
  // (see the table next to the NEUI_MK_* enum in <neui/d/events.h>).
  inline uint32_t mk_from_mac(uint64_t pressed_buttons, uint64_t flags)
  {
    uint32_t m = 0;
    if (pressed_buttons & ns_input::kLeft)  m |= NEUI_MK_LBUTTON;
    if (pressed_buttons & ns_input::kRight) m |= NEUI_MK_RBUTTON;
    if (flags & ns_input::kShift)   m |= NEUI_MK_SHIFT;
    if (flags & ns_input::kControl) m |= NEUI_MK_CONTROL;
    if (flags & ns_input::kOption)  m |= NEUI_MK_ALT;
    if (flags & ns_input::kCommand) m |= NEUI_MK_COMMAND;
    return m;
  }
}
