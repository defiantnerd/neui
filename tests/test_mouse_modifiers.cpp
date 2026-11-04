// Tier-1 coverage for the portable cores of the input-modifier translations:
//   hosts/shared/mouse_modifiers.h - OS state -> NEUI_MK_* (buttonmap)
//   hosts/shared/modifier_keys.h   - the modifier keys as NEUI_KEY_* KEYDOWN /
//                                    KEYUP, and the NEUI_KMOD_* state after one
//
// These exist as plain functions over integers precisely so this machine can test
// all three platforms' decoding: the Win32 / X11 / AppKit wrappers around them
// (hosts/shared/{win32,linux,macos}/keys_*.h) only read the live OS state, and
// static_assert that the raw constants used here are the SDK's own. The tests
// below therefore use the OS's numeric values directly (MK_XBUTTON1 = 0x0020,
// ShiftMask = 1, NSEventModifierFlagCommand = 1 << 20 ...), written out.

#include "neui_test.h"

#include "mouse_modifiers.h"
#include "modifier_keys.h"

using namespace neui_detail;

// ---------------------------------------------------------------------------
// The bit values themselves
// ---------------------------------------------------------------------------

TEST_CASE("NEUI_MK_*: Command sits above every Win32 MK_* bit, Alt where OLE has it")
{
  // MK_XBUTTON2 is the highest Win32 mouse-message key-state bit.
  const uint32_t win32_highest = 0x0040;
  CHECK((uint32_t)NEUI_MK_COMMAND > win32_highest);
  CHECK_EQ((uint32_t)NEUI_MK_COMMAND, 0x0080u);
  CHECK_EQ((uint32_t)NEUI_MK_ALT, 0x0020u);

  // All seven are distinct single bits.
  const uint32_t all[] = { NEUI_MK_LBUTTON, NEUI_MK_RBUTTON, NEUI_MK_SHIFT,
                           NEUI_MK_CONTROL, NEUI_MK_MBUTTON, NEUI_MK_ALT,
                           NEUI_MK_COMMAND };
  uint32_t seen = 0;
  for (uint32_t b : all) {
    CHECK_EQ(b & (b - 1), 0u);       // exactly one bit
    CHECK_EQ(seen & b, 0u);          // not used twice
    seen |= b;
  }
}

// ---------------------------------------------------------------------------
// Win32
// ---------------------------------------------------------------------------

TEST_CASE("mk_from_win32: buttons and Shift / Control pass through")
{
  CHECK_EQ(mk_from_win32(0x0001, false, false), (uint32_t)NEUI_MK_LBUTTON);
  CHECK_EQ(mk_from_win32(0x0002, false, false), (uint32_t)NEUI_MK_RBUTTON);
  CHECK_EQ(mk_from_win32(0x0004, false, false), (uint32_t)NEUI_MK_SHIFT);
  CHECK_EQ(mk_from_win32(0x0008, false, false), (uint32_t)NEUI_MK_CONTROL);
  CHECK_EQ(mk_from_win32(0x0010, false, false), (uint32_t)NEUI_MK_MBUTTON);
  CHECK_EQ(mk_from_win32(0x0001 | 0x0004 | 0x0008, false, false),
           (uint32_t)(NEUI_MK_LBUTTON | NEUI_MK_SHIFT | NEUI_MK_CONTROL));
}

TEST_CASE("mk_from_win32: the side buttons are masked, so XBUTTON1 is not a phantom Alt")
{
  const uint32_t MK_XBUTTON1 = 0x0020, MK_XBUTTON2 = 0x0040;
  CHECK_EQ(mk_from_win32(MK_XBUTTON1, false, false), 0u);
  CHECK_EQ(mk_from_win32(MK_XBUTTON2, false, false), 0u);
  CHECK_EQ(mk_from_win32(MK_XBUTTON1 | MK_XBUTTON2 | 0x0001, false, false),
           (uint32_t)NEUI_MK_LBUTTON);
  // Garbage in the high word (a wheel message's delta, if a caller forgot
  // GET_KEYSTATE_WPARAM) must not leak in either.
  CHECK_EQ(mk_from_win32(0x78000000u | 0x0004, false, false),
           (uint32_t)NEUI_MK_SHIFT);
}

TEST_CASE("mk_from_win32: Alt and the Windows key come from the live state, not the message")
{
  CHECK_EQ(mk_from_win32(0, true, false), (uint32_t)NEUI_MK_ALT);
  CHECK_EQ(mk_from_win32(0, false, true), (uint32_t)NEUI_MK_COMMAND);
  CHECK_EQ(mk_from_win32(0x0004 | 0x0010, true, true),
           (uint32_t)(NEUI_MK_SHIFT | NEUI_MK_MBUTTON | NEUI_MK_ALT | NEUI_MK_COMMAND));
  // And a side button next to a held Alt still reports exactly one Alt bit.
  CHECK_EQ(mk_from_win32(0x0020, true, false), (uint32_t)NEUI_MK_ALT);
}

TEST_CASE("mk_from_win32_ole: grfKeyState's 0x0020 IS Alt (MK_ALT), unlike a mouse message's")
{
  CHECK_EQ(mk_from_win32_ole(0x0020, false), (uint32_t)NEUI_MK_ALT);
  CHECK_EQ(mk_from_win32_ole(0x0001 | 0x0008 | 0x0020, false),
           (uint32_t)(NEUI_MK_LBUTTON | NEUI_MK_CONTROL | NEUI_MK_ALT));
  CHECK_EQ(mk_from_win32_ole(0, true), (uint32_t)NEUI_MK_COMMAND);
  // Nothing else is let through: 0x0040 is not an OLE key-state bit.
  CHECK_EQ(mk_from_win32_ole(0x0040 | 0x0080 | 0xFF00, false), 0u);
}

// ---------------------------------------------------------------------------
// X11
// ---------------------------------------------------------------------------

TEST_CASE("mk_from_x11: buttons, Shift, Control, Alt (Mod1), Command (Mod4)")
{
  // The X protocol's values: ShiftMask 1<<0, LockMask 1<<1, ControlMask 1<<2,
  // Mod1Mask 1<<3, Mod2Mask 1<<4 (NumLock), Mod4Mask 1<<6, Button1Mask 1<<8,
  // Button2Mask 1<<9, Button3Mask 1<<10.
  CHECK_EQ(mk_from_x11(1u << 0), (uint32_t)NEUI_MK_SHIFT);
  CHECK_EQ(mk_from_x11(1u << 2), (uint32_t)NEUI_MK_CONTROL);
  CHECK_EQ(mk_from_x11(1u << 3), (uint32_t)NEUI_MK_ALT);
  CHECK_EQ(mk_from_x11(1u << 6), (uint32_t)NEUI_MK_COMMAND);
  CHECK_EQ(mk_from_x11(1u << 8), (uint32_t)NEUI_MK_LBUTTON);
  CHECK_EQ(mk_from_x11(1u << 9), (uint32_t)NEUI_MK_MBUTTON);
  CHECK_EQ(mk_from_x11(1u << 10), (uint32_t)NEUI_MK_RBUTTON);   // X button 3 = right
  CHECK_EQ(mk_from_x11((1u << 3) | (1u << 6) | (1u << 8)),
           (uint32_t)(NEUI_MK_ALT | NEUI_MK_COMMAND | NEUI_MK_LBUTTON));
}

TEST_CASE("mk_from_x11: Caps Lock, NumLock and the other modifiers are ignored")
{
  const uint32_t lock = 1u << 1, mod2 = 1u << 4, mod3 = 1u << 5, mod5 = 1u << 7;
  CHECK_EQ(mk_from_x11(lock | mod2 | mod3 | mod5), 0u);
  // ...also with the real modifiers present: NumLock is nearly always on.
  CHECK_EQ(mk_from_x11(mod2 | (1u << 3)), (uint32_t)NEUI_MK_ALT);
  // Button4Mask / Button5Mask (1<<11, 1<<12) are the wheel's, not buttons here.
  CHECK_EQ(mk_from_x11((1u << 11) | (1u << 12)), 0u);
}

// ---------------------------------------------------------------------------
// macOS
// ---------------------------------------------------------------------------

namespace {
  // NSEventModifierFlag* values as AppKit defines them.
  constexpr uint64_t kShift = 1ull << 17, kControl = 1ull << 18,
                     kOption = 1ull << 19, kCommand = 1ull << 20;
  constexpr uint64_t kCapsLock = 1ull << 16, kFunction = 1ull << 23;
}

TEST_CASE("mk_from_mac: Option is Alt, Command is Command, Control is the physical Control")
{
  CHECK_EQ(mk_from_mac(0, kShift),   (uint32_t)NEUI_MK_SHIFT);
  CHECK_EQ(mk_from_mac(0, kControl), (uint32_t)NEUI_MK_CONTROL);
  CHECK_EQ(mk_from_mac(0, kOption),  (uint32_t)NEUI_MK_ALT);
  CHECK_EQ(mk_from_mac(0, kCommand), (uint32_t)NEUI_MK_COMMAND);
  CHECK_EQ(mk_from_mac(0, kOption | kCommand),
           (uint32_t)(NEUI_MK_ALT | NEUI_MK_COMMAND));
  // Cmd does NOT show up as Control, which is where NEUI_KMOD_CTRL differs.
  CHECK_EQ(mk_from_mac(0, kCommand) & NEUI_MK_CONTROL, 0u);
}

TEST_CASE("mk_from_mac: pressed buttons, and Caps Lock / Fn are not reported")
{
  CHECK_EQ(mk_from_mac(0x1, 0), (uint32_t)NEUI_MK_LBUTTON);
  CHECK_EQ(mk_from_mac(0x2, 0), (uint32_t)NEUI_MK_RBUTTON);
  CHECK_EQ(mk_from_mac(0x3, kOption),
           (uint32_t)(NEUI_MK_LBUTTON | NEUI_MK_RBUTTON | NEUI_MK_ALT));
  CHECK_EQ(mk_from_mac(0, kCapsLock | kFunction), 0u);
  // Device-dependent low bits of the flags word (NX_DEVICELSHIFTKEYMASK ...) are
  // not modifiers on their own.
  CHECK_EQ(mk_from_mac(0, 0x0000FFFFull), 0u);
}

// ---------------------------------------------------------------------------
// The modifier keys as keys
// ---------------------------------------------------------------------------

TEST_CASE("modifier key codes: identity-mapped on Windows, distinct, clear of the other keys")
{
  CHECK_EQ((uint32_t)NEUI_KEY_SHIFT,   0x10u);   // VK_SHIFT
  CHECK_EQ((uint32_t)NEUI_KEY_CONTROL, 0x11u);   // VK_CONTROL
  CHECK_EQ((uint32_t)NEUI_KEY_ALT,     0x12u);   // VK_MENU
  CHECK_EQ((uint32_t)NEUI_KEY_COMMAND, 0x5Bu);   // VK_LWIN
  // Nothing else in the enum shares those values.
  const uint32_t others[] = { NEUI_KEY_BACK, NEUI_KEY_TAB, NEUI_KEY_RETURN,
    NEUI_KEY_ESCAPE, NEUI_KEY_SPACE, NEUI_KEY_PAGEUP, NEUI_KEY_PAGEDOWN,
    NEUI_KEY_END, NEUI_KEY_HOME, NEUI_KEY_LEFT, NEUI_KEY_UP, NEUI_KEY_RIGHT,
    NEUI_KEY_DOWN, NEUI_KEY_INSERT, NEUI_KEY_DELETE, NEUI_KEY_0, NEUI_KEY_9,
    NEUI_KEY_A, NEUI_KEY_Z, NEUI_KEY_F1, NEUI_KEY_F12 };
  for (uint32_t k : others) {
    CHECK(k != (uint32_t)NEUI_KEY_SHIFT);
    CHECK(k != (uint32_t)NEUI_KEY_CONTROL);
    CHECK(k != (uint32_t)NEUI_KEY_ALT);
    CHECK(k != (uint32_t)NEUI_KEY_COMMAND);
  }
}

TEST_CASE("vk_to_modifier_key: both sides of each Win32 modifier fold to one code")
{
  CHECK_EQ(vk_to_modifier_key(0x10), (uint32_t)NEUI_KEY_SHIFT);     // VK_SHIFT
  CHECK_EQ(vk_to_modifier_key(0xA0), (uint32_t)NEUI_KEY_SHIFT);     // VK_LSHIFT
  CHECK_EQ(vk_to_modifier_key(0xA1), (uint32_t)NEUI_KEY_SHIFT);     // VK_RSHIFT
  CHECK_EQ(vk_to_modifier_key(0x11), (uint32_t)NEUI_KEY_CONTROL);   // VK_CONTROL
  CHECK_EQ(vk_to_modifier_key(0xA2), (uint32_t)NEUI_KEY_CONTROL);
  CHECK_EQ(vk_to_modifier_key(0xA3), (uint32_t)NEUI_KEY_CONTROL);
  CHECK_EQ(vk_to_modifier_key(0x12), (uint32_t)NEUI_KEY_ALT);       // VK_MENU
  CHECK_EQ(vk_to_modifier_key(0xA4), (uint32_t)NEUI_KEY_ALT);
  CHECK_EQ(vk_to_modifier_key(0xA5), (uint32_t)NEUI_KEY_ALT);
  CHECK_EQ(vk_to_modifier_key(0x5B), (uint32_t)NEUI_KEY_COMMAND);   // VK_LWIN
  CHECK_EQ(vk_to_modifier_key(0x5C), (uint32_t)NEUI_KEY_COMMAND);   // VK_RWIN
}

TEST_CASE("vk_to_modifier_key: every other virtual key is not a modifier")
{
  CHECK_EQ(vk_to_modifier_key('A'), 0u);
  CHECK_EQ(vk_to_modifier_key(0x20), 0u);   // VK_SPACE
  CHECK_EQ(vk_to_modifier_key(0x14), 0u);   // VK_CAPITAL
  CHECK_EQ(vk_to_modifier_key(0x5D), 0u);   // VK_APPS (the menu key)
  CHECK_EQ(vk_to_modifier_key(0x70), 0u);   // VK_F1
  CHECK_EQ(vk_to_modifier_key(0), 0u);
}

TEST_CASE("x11_keysym_to_modifier_key: Shift / Control / Alt+Meta / Super, either side")
{
  CHECK_EQ(x11_keysym_to_modifier_key(0xffe1), (uint32_t)NEUI_KEY_SHIFT);     // Shift_L
  CHECK_EQ(x11_keysym_to_modifier_key(0xffe2), (uint32_t)NEUI_KEY_SHIFT);     // Shift_R
  CHECK_EQ(x11_keysym_to_modifier_key(0xffe3), (uint32_t)NEUI_KEY_CONTROL);   // Control_L
  CHECK_EQ(x11_keysym_to_modifier_key(0xffe4), (uint32_t)NEUI_KEY_CONTROL);   // Control_R
  CHECK_EQ(x11_keysym_to_modifier_key(0xffe9), (uint32_t)NEUI_KEY_ALT);       // Alt_L
  CHECK_EQ(x11_keysym_to_modifier_key(0xffea), (uint32_t)NEUI_KEY_ALT);       // Alt_R
  CHECK_EQ(x11_keysym_to_modifier_key(0xffe7), (uint32_t)NEUI_KEY_ALT);       // Meta_L
  CHECK_EQ(x11_keysym_to_modifier_key(0xffe8), (uint32_t)NEUI_KEY_ALT);       // Meta_R
  CHECK_EQ(x11_keysym_to_modifier_key(0xffeb), (uint32_t)NEUI_KEY_COMMAND);   // Super_L
  CHECK_EQ(x11_keysym_to_modifier_key(0xffec), (uint32_t)NEUI_KEY_COMMAND);   // Super_R
}

TEST_CASE("x11_keysym_to_modifier_key: lock keys, AltGr, Hyper and ordinary keys are not modifiers")
{
  CHECK_EQ(x11_keysym_to_modifier_key(0xffe5), 0u);   // Caps_Lock
  CHECK_EQ(x11_keysym_to_modifier_key(0xffe6), 0u);   // Shift_Lock
  CHECK_EQ(x11_keysym_to_modifier_key(0xff7f), 0u);   // Num_Lock
  CHECK_EQ(x11_keysym_to_modifier_key(0xfe03), 0u);   // ISO_Level3_Shift (AltGr)
  CHECK_EQ(x11_keysym_to_modifier_key(0xffed), 0u);   // Hyper_L
  CHECK_EQ(x11_keysym_to_modifier_key('a'), 0u);
  CHECK_EQ(x11_keysym_to_modifier_key(0), 0u);
}

// ---------------------------------------------------------------------------
// macOS flagsChanged: diff
// ---------------------------------------------------------------------------

TEST_CASE("mac_modifier_key_changes: one modifier going down, then up")
{
  ModifierKeyChange c[4];
  REQUIRE_EQ(mac_modifier_key_changes(0, kOption, c), 1);
  CHECK_EQ(c[0].keycode, (uint32_t)NEUI_KEY_ALT);
  CHECK(c[0].down);

  REQUIRE_EQ(mac_modifier_key_changes(kOption, 0, c), 1);
  CHECK_EQ(c[0].keycode, (uint32_t)NEUI_KEY_ALT);
  CHECK_FALSE(c[0].down);
}

TEST_CASE("mac_modifier_key_changes: each of the four reports its own key")
{
  ModifierKeyChange c[4];
  REQUIRE_EQ(mac_modifier_key_changes(0, kShift, c), 1);
  CHECK_EQ(c[0].keycode, (uint32_t)NEUI_KEY_SHIFT);
  REQUIRE_EQ(mac_modifier_key_changes(0, kControl, c), 1);
  CHECK_EQ(c[0].keycode, (uint32_t)NEUI_KEY_CONTROL);
  REQUIRE_EQ(mac_modifier_key_changes(0, kCommand, c), 1);
  CHECK_EQ(c[0].keycode, (uint32_t)NEUI_KEY_COMMAND);
}

TEST_CASE("mac_modifier_key_changes: a flag that did not move reports nothing")
{
  ModifierKeyChange c[4];
  CHECK_EQ(mac_modifier_key_changes(0, 0, c), 0);
  // Option held, Command pressed: only Command is news.
  REQUIRE_EQ(mac_modifier_key_changes(kOption, kOption | kCommand, c), 1);
  CHECK_EQ(c[0].keycode, (uint32_t)NEUI_KEY_COMMAND);
  CHECK(c[0].down);
  // Caps Lock and Fn flip flagsChanged: too, but are not neui's modifiers.
  CHECK_EQ(mac_modifier_key_changes(0, kCapsLock, c), 0);
  CHECK_EQ(mac_modifier_key_changes(kShift, kShift | kFunction, c), 0);
}

TEST_CASE("mac_modifier_key_changes: several at once come out in a fixed order")
{
  ModifierKeyChange c[4];
  // Releasing Command while pressing Shift in one event (a chord handoff).
  REQUIRE_EQ(mac_modifier_key_changes(kCommand, kShift, c), 2);
  CHECK_EQ(c[0].keycode, (uint32_t)NEUI_KEY_SHIFT);
  CHECK(c[0].down);
  CHECK_EQ(c[1].keycode, (uint32_t)NEUI_KEY_COMMAND);
  CHECK_FALSE(c[1].down);

  // All four released together (the window lost them all at once).
  REQUIRE_EQ(mac_modifier_key_changes(kShift | kControl | kOption | kCommand, 0, c), 4);
  CHECK_EQ(c[0].keycode, (uint32_t)NEUI_KEY_SHIFT);
  CHECK_EQ(c[1].keycode, (uint32_t)NEUI_KEY_CONTROL);
  CHECK_EQ(c[2].keycode, (uint32_t)NEUI_KEY_ALT);
  CHECK_EQ(c[3].keycode, (uint32_t)NEUI_KEY_COMMAND);
  for (int i = 0; i < 4; ++i) CHECK_FALSE(c[i].down);
}

// ---------------------------------------------------------------------------
// NEUI_KMOD_* state after a modifier key
// ---------------------------------------------------------------------------

TEST_CASE("kmod_bit_for_modifier_key: Control and Command trade roles between macOS and the rest")
{
  CHECK_EQ(kmod_bit_for_modifier_key(NEUI_KEY_SHIFT, false), (uint32_t)NEUI_KMOD_SHIFT);
  CHECK_EQ(kmod_bit_for_modifier_key(NEUI_KEY_SHIFT, true),  (uint32_t)NEUI_KMOD_SHIFT);
  CHECK_EQ(kmod_bit_for_modifier_key(NEUI_KEY_ALT, false),   (uint32_t)NEUI_KMOD_ALT);
  CHECK_EQ(kmod_bit_for_modifier_key(NEUI_KEY_ALT, true),    (uint32_t)NEUI_KMOD_ALT);
  // Windows / Linux: Control is the primary modifier, Win / Super the secondary.
  CHECK_EQ(kmod_bit_for_modifier_key(NEUI_KEY_CONTROL, false), (uint32_t)NEUI_KMOD_CTRL);
  CHECK_EQ(kmod_bit_for_modifier_key(NEUI_KEY_COMMAND, false), (uint32_t)NEUI_KMOD_META);
  // macOS: Command is the primary modifier, Control the secondary.
  CHECK_EQ(kmod_bit_for_modifier_key(NEUI_KEY_COMMAND, true),  (uint32_t)NEUI_KMOD_CTRL);
  CHECK_EQ(kmod_bit_for_modifier_key(NEUI_KEY_CONTROL, true),  (uint32_t)NEUI_KMOD_META);
  // Not a modifier.
  CHECK_EQ(kmod_bit_for_modifier_key(NEUI_KEY_A, false), 0u);
  CHECK_EQ(kmod_bit_for_modifier_key(0, true), 0u);
}

TEST_CASE("kmod_after_modifier_key: the state AFTER the change, from X11's state BEFORE it")
{
  // X11: a Shift press arrives with the Shift bit still clear...
  CHECK_EQ(kmod_after_modifier_key(0, NEUI_KEY_SHIFT, true, false),
           (uint32_t)NEUI_KMOD_SHIFT);
  // ...and its release with the bit still set.
  CHECK_EQ(kmod_after_modifier_key(NEUI_KMOD_SHIFT, NEUI_KEY_SHIFT, false, false), 0u);
  // Other held modifiers are left alone.
  CHECK_EQ(kmod_after_modifier_key(NEUI_KMOD_SHIFT | NEUI_KMOD_ALT, NEUI_KEY_ALT, false, false),
           (uint32_t)NEUI_KMOD_SHIFT);
  CHECK_EQ(kmod_after_modifier_key(NEUI_KMOD_SHIFT, NEUI_KEY_COMMAND, true, false),
           (uint32_t)(NEUI_KMOD_SHIFT | NEUI_KMOD_META));
  // Idempotent: a press of a modifier already in the state changes nothing.
  CHECK_EQ(kmod_after_modifier_key(NEUI_KMOD_CTRL, NEUI_KEY_CONTROL, true, false),
           (uint32_t)NEUI_KMOD_CTRL);
  // A non-modifier key is no change at all.
  CHECK_EQ(kmod_after_modifier_key(NEUI_KMOD_SHIFT, NEUI_KEY_A, true, false),
           (uint32_t)NEUI_KMOD_SHIFT);
}
