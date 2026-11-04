#pragma once

#ifdef _WIN32

#include <neui/neui.h>

#include <windows.h>

#include <cstdint>

#include "../mouse_modifiers.h"
#include "../modifier_keys.h"

// Win32 input-modifier translation primitives shared by BOTH win32 hosts (the
// native HWND host and the crossplatform host's win32 platform layer).
// Completes the set alongside hosts/shared/macos/keys_macos.h and
// hosts/shared/linux/keys_linux.h:
//   - win32_buttonmap:            WPARAM key state -> NEUI_MK_* bitmask.
//   - win32_buttonmap_from_state: live key state   -> NEUI_MK_* bitmask.
//   - win32_buttonmap_from_ole:   grfKeyState      -> NEUI_MK_* bitmask.
//   - win32_kmod_from_state:      live key state   -> NEUI_KMOD_* bitmask.
//   - win32_keycode_to_neui /
//     win32_modifier_key /
//     win32_key_is_repeat:        WM_[SYS]KEYDOWN / UP -> NEUI_KEY_* codes,
//                                 including the modifier keys as keys.
//
// Prefer the WPARAM form wherever the message carries key state (WM_MOUSEMOVE,
// WM_?BUTTON*, and - via GET_KEYSTATE_WPARAM - the wheel messages): that is
// the state as of when the message was POSTED, whereas GetKeyState reflects
// now, which can differ on a backed-up queue. The *_from_state forms exist for
// the messages that carry none (WM_MOUSELEAVE, WM_KEYDOWN / WM_KEYUP).
//
// A mouse message's key state has no Alt bit and no Windows-key bit, so even
// the WPARAM form adds those two from GetKeyState. The decoding itself is the
// portable, Tier-1 tested hosts/shared/mouse_modifiers.h (mk_from_win32 /
// mk_from_win32_ole); these wrappers only read the live key state.

namespace neui_detail
{

// NEUI_MK_LBUTTON .. NEUI_MK_MBUTTON numerically match the Win32 MK_* constants
// (see the note on the enum in <neui/d/events.h>), which the mask in
// mk_from_win32 relies on - so state it where a Windows compiler checks it. The
// bit NEUI_MK_COMMAND added must stay clear of every raw MK_* value.
static_assert(NEUI_MK_LBUTTON == MK_LBUTTON && NEUI_MK_RBUTTON == MK_RBUTTON
           && NEUI_MK_SHIFT   == MK_SHIFT   && NEUI_MK_CONTROL == MK_CONTROL
           && NEUI_MK_MBUTTON == MK_MBUTTON,
              "NEUI_MK_* no longer match Win32 MK_*; mk_from_win32 masks on that");
// 0x0040 is MK_XBUTTON2, the highest Win32 mouse key-state bit (spelled out so
// this does not depend on _WIN32_WINNT exposing the MK_XBUTTON macros).
static_assert(NEUI_MK_COMMAND > 0x0040 && (NEUI_MK_COMMAND & (NEUI_MK_COMMAND - 1)) == 0,
              "NEUI_MK_COMMAND must be one bit above every Win32 MK_* bit");

// GetKeyState is the key's state as of the last input message this thread's
// queue handed out - the same source win32_kmod_from_state uses.
inline bool win32_key_down(int vk)
{
  return (GetKeyState(vk) & 0x8000) != 0;
}

inline bool win32_alt_down()
{
  return win32_key_down(VK_MENU);
}

// The Windows key (NEUI_MK_COMMAND / NEUI_KMOD_META). Best-effort: the shell
// owns this key, and GetKeyState does not always see it while it does.
inline bool win32_command_down()
{
  return win32_key_down(VK_LWIN) || win32_key_down(VK_RWIN);
}

// The buttons + Shift / Control come from the message's own key state; Alt and
// the Windows key are not in it and are read live. The state is masked, never
// forwarded: MK_XBUTTON1 is 0x0020, the bit NEUI_MK_ALT uses.
inline uint32_t win32_buttonmap(WPARAM key_state)
{
  return mk_from_win32(static_cast<uint32_t>(key_state),
                       win32_alt_down(), win32_command_down());
}

// OLE drag-and-drop's grfKeyState (IDropTarget::DragEnter / DragOver / Drop).
// Unlike a mouse message it carries MK_ALT (0x0020), so Alt comes from the drag
// loop's own reading rather than from this thread's GetKeyState; only the
// Windows key, which grfKeyState does not carry, is read live.
inline uint32_t win32_buttonmap_from_ole(DWORD grf_key_state)
{
  return mk_from_win32_ole(static_cast<uint32_t>(grf_key_state),
                           win32_command_down());
}

inline uint32_t win32_buttonmap_from_state()
{
  uint32_t m = 0;
  if (win32_key_down(VK_LBUTTON)) m |= NEUI_MK_LBUTTON;
  if (win32_key_down(VK_RBUTTON)) m |= NEUI_MK_RBUTTON;
  if (win32_key_down(VK_MBUTTON)) m |= NEUI_MK_MBUTTON;
  if (win32_key_down(VK_SHIFT))   m |= NEUI_MK_SHIFT;
  if (win32_key_down(VK_CONTROL)) m |= NEUI_MK_CONTROL;
  if (win32_alt_down())           m |= NEUI_MK_ALT;
  if (win32_command_down())       m |= NEUI_MK_COMMAND;
  return m;
}

// The modifier keys as KEYS (NEUI_KEY_SHIFT / _CONTROL / _ALT / _COMMAND; the
// contract is in hosts/shared/modifier_keys.h). The generic codes are identity-
// mapped - that is the point of their values - and the right Windows key and the
// left / right variants fold onto them; the portable table does the folding.
static_assert(NEUI_KEY_SHIFT == VK_SHIFT && NEUI_KEY_CONTROL == VK_CONTROL
           && NEUI_KEY_ALT == VK_MENU && NEUI_KEY_COMMAND == VK_LWIN,
              "NEUI_KEY_* modifier codes no longer match the Win32 VK_*");

// The NEUI_KEY_* a WM_[SYS]KEYDOWN / WM_[SYS]KEYUP wParam stands for when it is
// a modifier key (either side), else 0.
inline uint32_t win32_modifier_key(WPARAM vk)
{
  return vk_to_modifier_key(static_cast<uint32_t>(vk));
}

// A key message's wParam as a NEUI_KEY_*: every key is identity-mapped except
// that the modifiers are folded to their one code (VK_RWIN -> NEUI_KEY_COMMAND,
// VK_LSHIFT -> NEUI_KEY_SHIFT ...).
inline uint32_t win32_keycode_to_neui(WPARAM vk)
{
  const uint32_t mod = win32_modifier_key(vk);
  return mod != 0 ? mod : static_cast<uint32_t>(vk);
}

// True for a typematic repeat of WM_[SYS]KEYDOWN: bit 30 of lParam is the
// key's previous state. A modifier held down repeats like any other key;
// modifier KEYDOWNs are delivered once, so callers drop the repeats.
inline bool win32_key_is_repeat(LPARAM lp)
{
  return (static_cast<uint32_t>(lp) & (1u << 30)) != 0;
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
  if (win32_command_down())             m |= NEUI_KMOD_META;
  return m;
}

} // namespace neui_detail

#endif // _WIN32
