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

  NEUI_KEY_F1 = 0x70, NEUI_KEY_F2, NEUI_KEY_F3,  NEUI_KEY_F4,
  NEUI_KEY_F5,        NEUI_KEY_F6, NEUI_KEY_F7,  NEUI_KEY_F8,
  NEUI_KEY_F9,        NEUI_KEY_F10, NEUI_KEY_F11, NEUI_KEY_F12,
} neui_key_t;

// Keyboard modifier bitmask carried by NEUI_EVENT_KEYDOWN's `modifiers` and
// used to define accelerator shortcuts. NEUI_KMOD_CTRL maps to the
// platform's primary command modifier (Cmd on macOS, Ctrl on Win/Linux);
// NEUI_KMOD_META is the secondary (Ctrl on macOS, Win/Super on Win/Linux).
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
