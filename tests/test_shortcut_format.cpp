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

// ---------------------------------------------------------------------------
// macOS glyph style. Used by the NEUI_W_POPUPMENU cascade, which is the first
// macOS surface that DRAWS the label itself (the native NSMenu path strips it
// and shows keyEquivalent instead), so a Mac must not read "Win+Z".

static std::string mfmt(uint32_t mods, uint32_t key)
{
  return format_shortcut_label_mac(mods, key);
}

TEST_CASE("shortcut mac: NEUI_KEY_NONE yields an empty label")
{
  CHECK_EQ(mfmt(NEUI_KMOD_META, NEUI_KEY_NONE), std::string(""));
}

TEST_CASE("shortcut mac: META is Command, and there are no separators")
{
  CHECK_EQ(mfmt(NEUI_KMOD_META, NEUI_KEY_S), std::string("\xE2\x8C\x98" "S"));
  // CTRL stays Control (the caret), NOT Command - a client that means Cmd binds
  // NEUI_KMOD_META. Getting this backwards would silently mislabel every
  // Ctrl-bound shortcut on a Mac.
  CHECK_EQ(mfmt(NEUI_KMOD_CTRL, NEUI_KEY_S), std::string("\xE2\x8C\x83" "S"));
  CHECK_EQ(mfmt(NEUI_KMOD_ALT, NEUI_KEY_S), std::string("\xE2\x8C\xA5" "S"));
  CHECK_EQ(mfmt(NEUI_KMOD_SHIFT, NEUI_KEY_S), std::string("\xE2\x87\xA7" "S"));
}

TEST_CASE("shortcut mac: modifier order is Control, Option, Shift, Command")
{
  // Apple's fixed order (HIG "Modifier Keys"), which is NOT the order the bits
  // are declared in and NOT the Windows order.
  CHECK_EQ(mfmt(NEUI_KMOD_META | NEUI_KMOD_SHIFT, NEUI_KEY_Z),
           std::string("\xE2\x87\xA7\xE2\x8C\x98" "Z"));
  CHECK_EQ(mfmt(NEUI_KMOD_CTRL | NEUI_KMOD_ALT | NEUI_KMOD_SHIFT | NEUI_KMOD_META,
                NEUI_KEY_A),
           std::string("\xE2\x8C\x83\xE2\x8C\xA5\xE2\x87\xA7\xE2\x8C\x98" "A"));
}

TEST_CASE("shortcut mac: symbol keys, named keys, digits, hex fallback")
{
  CHECK_EQ(mfmt(NEUI_KMOD_META, NEUI_KEY_LEFT),
           std::string("\xE2\x8C\x98\xE2\x86\x90"));
  CHECK_EQ(mfmt(0, NEUI_KEY_RETURN), std::string("\xE2\x86\xA9"));
  CHECK_EQ(mfmt(0, NEUI_KEY_BACK),   std::string("\xE2\x8C\xAB"));
  // Words, not glyphs, for the less-recognisable ones.
  CHECK_EQ(mfmt(0, NEUI_KEY_F11),  std::string("F11"));
  CHECK_EQ(mfmt(0, NEUI_KEY_HOME), std::string("Home"));
  CHECK_EQ(mfmt(0, NEUI_KEY_0),    std::string("0"));
  // No trailing separator to strip on the hex path (unlike the win formatter).
  CHECK_EQ(mfmt(NEUI_KMOD_META, 0x99), std::string("\xE2\x8C\x98" "0x99"));
}

TEST_CASE("shortcut: the platform picker resolves to exactly one style")
{
  // Whichever way it resolves, it must equal one of the two concrete
  // formatters - a mismatch means the #if picked nothing.
  const std::string got  = format_shortcut_label(NEUI_KMOD_META, NEUI_KEY_Z);
#if defined(__APPLE__)
  CHECK_EQ(got, mfmt(NEUI_KMOD_META, NEUI_KEY_Z));
  CHECK(got != fmt(NEUI_KMOD_META, NEUI_KEY_Z));   // and it is NOT "Win+Z"
#else
  CHECK_EQ(got, fmt(NEUI_KMOD_META, NEUI_KEY_Z));
  CHECK(got != mfmt(NEUI_KMOD_META, NEUI_KEY_Z));
#endif
}
