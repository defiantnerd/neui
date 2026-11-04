#pragma once

#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Public key codes used by NEUI_EVENT_KEYDOWN's `keycode` and by accelerator
// shortcut bindings (tree->set_shortcut). Values intentionally match Win32
// VK_* so the platform layer is identity-mapped on Windows; macOS / Linux
// hosts translate at the platform layer.
typedef enum neui_key
{
  NEUI_KEY_NONE   = 0x00,

  NEUI_KEY_BACK   = 0x08,  // Backspace
  NEUI_KEY_TAB    = 0x09,
  NEUI_KEY_RETURN = 0x0D,  // Enter

  // The modifier keys themselves, reported as NEUI_EVENT_KEYDOWN / _KEYUP when
  // one is pressed or released on its own (see "Modifier keys" below). Names the
  // PHYSICAL key, like NEUI_MK_* in d/events.h - NOT the role NEUI_KMOD_* has:
  // NEUI_KEY_CONTROL is the Control key on every platform, NEUI_KEY_COMMAND the
  // Command key on macOS and the key in the same place elsewhere (the Windows
  // key on Windows, Super on Linux). The left and right key of a kind share one
  // code. 0x10 / 0x11 / 0x12 / 0x5B are Win32 VK_SHIFT / VK_CONTROL / VK_MENU /
  // VK_LWIN, so they are identity-mapped on Windows like the rest.
  NEUI_KEY_SHIFT   = 0x10,
  NEUI_KEY_CONTROL = 0x11,
  NEUI_KEY_ALT     = 0x12,  // Alt on Windows / Linux, Option on macOS

  NEUI_KEY_ESCAPE = 0x1B,
  NEUI_KEY_SPACE  = 0x20,

  NEUI_KEY_PAGEUP   = 0x21,
  NEUI_KEY_PAGEDOWN = 0x22,
  NEUI_KEY_END    = 0x23,
  NEUI_KEY_HOME   = 0x24,
  NEUI_KEY_LEFT   = 0x25,
  NEUI_KEY_UP     = 0x26,
  NEUI_KEY_RIGHT  = 0x27,
  NEUI_KEY_DOWN   = 0x28,

  NEUI_KEY_INSERT = 0x2D,
  NEUI_KEY_DELETE = 0x2E,

  NEUI_KEY_0 = 0x30, NEUI_KEY_1, NEUI_KEY_2, NEUI_KEY_3, NEUI_KEY_4,
  NEUI_KEY_5,        NEUI_KEY_6, NEUI_KEY_7, NEUI_KEY_8, NEUI_KEY_9,

  NEUI_KEY_A = 0x41, NEUI_KEY_B, NEUI_KEY_C, NEUI_KEY_D, NEUI_KEY_E,
  NEUI_KEY_F,        NEUI_KEY_G, NEUI_KEY_H, NEUI_KEY_I, NEUI_KEY_J,
  NEUI_KEY_K,        NEUI_KEY_L, NEUI_KEY_M, NEUI_KEY_N, NEUI_KEY_O,
  NEUI_KEY_P,        NEUI_KEY_Q, NEUI_KEY_R, NEUI_KEY_S, NEUI_KEY_T,
  NEUI_KEY_U,        NEUI_KEY_V, NEUI_KEY_W, NEUI_KEY_X, NEUI_KEY_Y,
  NEUI_KEY_Z,

  NEUI_KEY_COMMAND = 0x5B,  // Command (macOS), Windows key, Super - see NEUI_KEY_SHIFT

  NEUI_KEY_F1 = 0x70, NEUI_KEY_F2, NEUI_KEY_F3,  NEUI_KEY_F4,
  NEUI_KEY_F5,        NEUI_KEY_F6, NEUI_KEY_F7,  NEUI_KEY_F8,
  NEUI_KEY_F9,        NEUI_KEY_F10, NEUI_KEY_F11, NEUI_KEY_F12,
} neui_key_t;

// Keyboard modifier bitmask carried by NEUI_EVENT_KEYDOWN's `modifiers` and
// used to define accelerator shortcuts. NEUI_KMOD_CTRL maps to the
// platform's primary command modifier (Cmd on macOS, Ctrl on Win/Linux);
// NEUI_KMOD_META is the secondary (Ctrl on macOS, Win/Super on Win/Linux).
//
// Modifier keys. A modifier pressed or released on its own is an ordinary key
// event for the focused widget: NEUI_EVENT_KEYDOWN / NEUI_EVENT_KEYUP with
// keycode NEUI_KEY_SHIFT / _CONTROL / _ALT / _COMMAND, so a client can follow
// them while the mouse is not moving - an Option / Alt change during a drag, say.
//   - `modifiers` is the NEUI_KMOD_* state AFTER the change: the KEYDOWN of
//     Shift already has NEUI_KMOD_SHIFT, its KEYUP no longer does. (The keycodes
//     name physical keys while the bits name roles - on macOS the Command key is
//     NEUI_KMOD_CTRL; the table next to NEUI_MK_* in d/events.h lines them up.)
//   - A held modifier does not repeat: one KEYDOWN, one KEYUP.
//   - The left and right key of a kind share one code. With both held, Windows
//     and Linux report each key's press and release (so a second KEYDOWN can
//     arrive while the first is still down), macOS only the change of the
//     combined state. Read `modifiers` for the truth, do not count events.
//   - There is never a NEUI_EVENT_KEYCHAR for them, and the built-in widgets
//     ignore them. A release that happens while the window is not receiving keys
//     is not delivered; neither is any modifier on iOS.
typedef enum neui_keymod
{
  NEUI_KMOD_NONE  = 0,
  NEUI_KMOD_SHIFT = 0x0001,
  NEUI_KMOD_CTRL  = 0x0002,
  NEUI_KMOD_ALT   = 0x0004,
  NEUI_KMOD_META  = 0x0008,
} neui_keymod_t;

#ifdef __cplusplus
}
#endif
