#include "neui_test.h"

#include "web/keys_web.h"

using namespace neui_detail;

TEST_CASE("keys_web: letters follow the layout (key), not the position (code)")
{
  // German QWERTZ: the key in the US "Y" position is labelled Z.
  CHECK_EQ(web_key_to_neui("KeyY", "z"), (uint32_t)NEUI_KEY_Z);
  CHECK_EQ(web_key_to_neui("KeyZ", "y"), (uint32_t)NEUI_KEY_Y);
  // Shift / Caps give upper case; same key.
  CHECK_EQ(web_key_to_neui("KeyA", "A"), (uint32_t)NEUI_KEY_A);
  // Cyrillic: no Latin letter in `key`, so the physical position decides.
  CHECK_EQ(web_key_to_neui("KeyC", "\xD1\x81"), (uint32_t)NEUI_KEY_C);
}

TEST_CASE("keys_web: digits come from code, so shifted digits stay digits")
{
  CHECK_EQ(web_key_to_neui("Digit7", "/"), (uint32_t)NEUI_KEY_7);   // Shift+7 on QWERTZ
  CHECK_EQ(web_key_to_neui("Digit0", "0"), (uint32_t)NEUI_KEY_0);
  CHECK_EQ(web_key_to_neui("Numpad3", "3"), (uint32_t)NEUI_KEY_3);
}

TEST_CASE("keys_web: named keys and function keys")
{
  CHECK_EQ(web_key_to_neui("Enter", "Enter"), (uint32_t)NEUI_KEY_RETURN);
  CHECK_EQ(web_key_to_neui("NumpadEnter", "Enter"), (uint32_t)NEUI_KEY_RETURN);
  CHECK_EQ(web_key_to_neui("Tab", "Tab"), (uint32_t)NEUI_KEY_TAB);
  CHECK_EQ(web_key_to_neui("Space", " "), (uint32_t)NEUI_KEY_SPACE);
  CHECK_EQ(web_key_to_neui("ArrowLeft", "ArrowLeft"), (uint32_t)NEUI_KEY_LEFT);
  CHECK_EQ(web_key_to_neui("Delete", "Delete"), (uint32_t)NEUI_KEY_DELETE);
  CHECK_EQ(web_key_to_neui("Backspace", "Backspace"), (uint32_t)NEUI_KEY_BACK);
  CHECK_EQ(web_key_to_neui("F1", "F1"), (uint32_t)NEUI_KEY_F1);
  CHECK_EQ(web_key_to_neui("F12", "F12"), (uint32_t)NEUI_KEY_F12);
  CHECK_EQ(web_key_to_neui("F13", "F13"), (uint32_t)0);
  CHECK_EQ(web_key_to_neui("ShiftLeft", "Shift"), (uint32_t)0);
  CHECK_EQ(web_key_to_neui("Minus", "-"), (uint32_t)0);
}

TEST_CASE("keys_web: the primary modifier is Cmd on Apple, Ctrl elsewhere")
{
  CHECK_EQ(web_modifiers_to_neui(WEB_MOD_CTRL, false), (uint32_t)NEUI_KMOD_CTRL);
  CHECK_EQ(web_modifiers_to_neui(WEB_MOD_META, false), (uint32_t)NEUI_KMOD_META);
  CHECK_EQ(web_modifiers_to_neui(WEB_MOD_META, true),  (uint32_t)NEUI_KMOD_CTRL);
  CHECK_EQ(web_modifiers_to_neui(WEB_MOD_CTRL, true),  (uint32_t)NEUI_KMOD_META);
  CHECK_EQ(web_modifiers_to_neui(WEB_MOD_SHIFT | WEB_MOD_ALT, false),
           (uint32_t)(NEUI_KMOD_SHIFT | NEUI_KMOD_ALT));
}

TEST_CASE("keys_web: AltGr types a character and is not Ctrl+Alt")
{
  // Windows reports AltGr+Q on a German keyboard as Ctrl + Alt + AltGraph, key "@".
  const uint32_t mods = web_modifiers_to_neui(WEB_MOD_CTRL | WEB_MOD_ALT | WEB_MOD_ALTGRAPH, false);
  CHECK_EQ(mods, (uint32_t)0);
  CHECK_EQ(web_key_char("@", mods), (uint32_t)'@');
}

TEST_CASE("keys_web: characters, shortcuts and key names")
{
  CHECK_EQ(web_key_char("a", 0), (uint32_t)'a');
  CHECK_EQ(web_key_char("A", NEUI_KMOD_SHIFT), (uint32_t)'A');
  CHECK_EQ(web_key_char("\xC3\xA4", 0), (uint32_t)0xE4);           // a-umlaut
  CHECK_EQ(web_key_char("\xF0\x9F\x98\x80", 0), (uint32_t)0x1F600); // emoji (4 bytes)
  CHECK_EQ(web_key_char("z", NEUI_KMOD_CTRL), (uint32_t)0);         // Ctrl+Z is a shortcut
  CHECK_EQ(web_key_char("Enter", 0), (uint32_t)0);
  CHECK_EQ(web_key_char("Dead", 0), (uint32_t)0);
  CHECK_EQ(web_key_char("", 0), (uint32_t)0);
  CHECK_EQ(web_single_codepoint("ab"), (uint32_t)0);
}

TEST_CASE("keys_web: pointer buttons and modifiers map to MK_* bits")
{
  CHECK_EQ(web_buttonmap(1, 0), (uint32_t)0x0001);
  CHECK_EQ(web_buttonmap(2, 0), (uint32_t)0x0002);
  CHECK_EQ(web_buttonmap(4, WEB_MOD_SHIFT | WEB_MOD_CTRL), (uint32_t)(0x0010 | 0x0004 | 0x0008));
}
