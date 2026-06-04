#include "neui_test.h"

#include "shortcut_format.h"

using namespace neui_detail;

static std::string fmt(uint32_t mods, uint32_t key)
{
  return format_shortcut_label_win(mods, key);
}

TEST_CASE("shortcut: NEUI_KEY_NONE yields an empty label")
{
  CHECK_EQ(fmt(NEUI_KMOD_CTRL, NEUI_KEY_NONE), std::string(""));
}

TEST_CASE("shortcut: single modifier + letter")
{
  CHECK_EQ(fmt(NEUI_KMOD_CTRL, NEUI_KEY_S), std::string("Ctrl+S"));
  CHECK_EQ(fmt(NEUI_KMOD_ALT, NEUI_KEY_F4), std::string("Alt+F4"));
}

TEST_CASE("shortcut: modifier ordering is Ctrl, Alt, Shift, Win")
{
  CHECK_EQ(fmt(NEUI_KMOD_CTRL | NEUI_KMOD_SHIFT, NEUI_KEY_Z),
           std::string("Ctrl+Shift+Z"));
  CHECK_EQ(fmt(NEUI_KMOD_CTRL | NEUI_KMOD_ALT | NEUI_KMOD_SHIFT, NEUI_KEY_A),
           std::string("Ctrl+Alt+Shift+A"));
}

TEST_CASE("shortcut: named keys, digits, no modifier")
{
  CHECK_EQ(fmt(0, NEUI_KEY_F11), std::string("F11"));
  CHECK_EQ(fmt(0, NEUI_KEY_0), std::string("0"));
  CHECK_EQ(fmt(0, NEUI_KEY_HOME), std::string("Home"));
  CHECK_EQ(fmt(0, NEUI_KEY_RETURN), std::string("Enter"));
}

TEST_CASE("shortcut: unknown key falls back to hex")
{
  // 0x99 is not a named key, digit, or letter -> hex display.
  CHECK_EQ(fmt(0, 0x99), std::string("0x99"));
}
