#pragma once

#include <neui/neui.h>

#include <cstdint>
#include <cstring>

// Browser keyboard-event translation for the crossplatform host's web platform
// layer (plans/wasm-host.md). Mirror of hosts/shared/linux/keys_linux.h /
// macos/keys_macos.h, but pure C++ over the strings a KeyboardEvent carries,
// so it is unit-tested (tests/test_keys_web.cpp) and compiles everywhere.
//
//   - web_key_to_neui:      (KeyboardEvent.code, .key) -> neui_key_t.
//   - web_modifiers_to_neui: physical modifier bits -> NEUI_KMOD_*, with the
//     Apple convention (Cmd is the primary modifier, NEUI_KMOD_CTRL).
//   - web_key_char:         the codepoint a key press types, if any.
//
// Letters come from `key`, not `code`: `code` names the physical position
// (KeyZ is the key labelled Y on a German keyboard), but neui keys follow the
// layout like Win32 virtual keys do, so Ctrl+Z must be the key labelled Z.
// Layouts without Latin letters (Cyrillic, Greek) fall back to `code`, as
// Win32 does. Digits, function and navigation keys come from `code`.

namespace neui_detail
{

// Physical modifier bits the JS side packs from a KeyboardEvent /
// PointerEvent / WheelEvent.
enum : uint32_t {
  WEB_MOD_SHIFT    = 0x01,
  WEB_MOD_CTRL     = 0x02,
  WEB_MOD_ALT      = 0x04,
  WEB_MOD_META     = 0x08,   // Cmd on Apple, Win / Super elsewhere
  WEB_MOD_ALTGRAPH = 0x10,   // getModifierState('AltGraph'): AltGr on Windows
};

// The UTF-8 string holds exactly one codepoint; returns it, else 0.
inline uint32_t web_single_codepoint(const char* s)
{
  if (!s || !*s) return 0;
  const unsigned char* p = reinterpret_cast<const unsigned char*>(s);
  uint32_t cp = 0;
  int n = 0;
  if (p[0] < 0x80)                { cp = p[0];        n = 1; }
  else if ((p[0] & 0xE0) == 0xC0) { cp = p[0] & 0x1F; n = 2; }
  else if ((p[0] & 0xF0) == 0xE0) { cp = p[0] & 0x0F; n = 3; }
  else if ((p[0] & 0xF8) == 0xF0) { cp = p[0] & 0x07; n = 4; }
  else return 0;
  for (int i = 1; i < n; ++i) {
    if ((p[i] & 0xC0) != 0x80) return 0;
    cp = (cp << 6) | (p[i] & 0x3F);
  }
  return p[n] == 0 ? cp : 0;
}

inline uint32_t web_key_to_neui(const char* code, const char* key)
{
  // Layout-mapped letters (what the key is labelled).
  const uint32_t k = web_single_codepoint(key);
  if (k >= 'a' && k <= 'z') return NEUI_KEY_A + (k - 'a');
  if (k >= 'A' && k <= 'Z') return NEUI_KEY_A + (k - 'A');
  if (!code) return 0;

  // Physical letters for non-Latin layouts; top-row and numpad digits.
  if (std::strncmp(code, "Key", 3) == 0 && code[3] >= 'A' && code[3] <= 'Z' && !code[4])
    return NEUI_KEY_A + static_cast<uint32_t>(code[3] - 'A');
  if (std::strncmp(code, "Digit", 5) == 0 && code[5] >= '0' && code[5] <= '9' && !code[6])
    return NEUI_KEY_0 + static_cast<uint32_t>(code[5] - '0');
  if (std::strncmp(code, "Numpad", 6) == 0 && code[6] >= '0' && code[6] <= '9' && !code[7])
    return NEUI_KEY_0 + static_cast<uint32_t>(code[6] - '0');
  if (code[0] == 'F' && code[1] >= '1' && code[1] <= '9') {
    int n = code[1] - '0';
    if (code[2] >= '0' && code[2] <= '9' && !code[3]) n = n * 10 + (code[2] - '0');
    else if (code[2]) n = 0;
    if (n >= 1 && n <= 12) return NEUI_KEY_F1 + static_cast<uint32_t>(n - 1);
  }

  struct Named { const char* code; uint32_t key; };
  static const Named k_named[] = {
    { "Backspace",   NEUI_KEY_BACK },
    { "Tab",         NEUI_KEY_TAB },
    { "Enter",       NEUI_KEY_RETURN },
    { "NumpadEnter", NEUI_KEY_RETURN },
    { "Escape",      NEUI_KEY_ESCAPE },
    { "Space",       NEUI_KEY_SPACE },
    { "PageUp",      NEUI_KEY_PAGEUP },
    { "PageDown",    NEUI_KEY_PAGEDOWN },
    { "End",         NEUI_KEY_END },
    { "Home",        NEUI_KEY_HOME },
    { "ArrowLeft",   NEUI_KEY_LEFT },
    { "ArrowUp",     NEUI_KEY_UP },
    { "ArrowRight",  NEUI_KEY_RIGHT },
    { "ArrowDown",   NEUI_KEY_DOWN },
    { "Insert",      NEUI_KEY_INSERT },
    { "Delete",      NEUI_KEY_DELETE },
    { "NumpadDecimal", 0 },   // types a character; no key of its own
  };
  for (const Named& n : k_named)
    if (std::strcmp(code, n.code) == 0) return n.key;
  return 0;
}

// Physical bits -> NEUI_KMOD_*. NEUI_KMOD_CTRL is the platform's primary
// modifier: Cmd on Apple, Ctrl elsewhere; NEUI_KMOD_META the secondary.
// AltGr (Windows reports it as Ctrl + Alt) is no modifier at all: it selects
// a character, which web_key_char delivers.
inline uint32_t web_modifiers_to_neui(uint32_t web, bool apple)
{
  if (web & WEB_MOD_ALTGRAPH) web &= ~(WEB_MOD_CTRL | WEB_MOD_ALT);
  uint32_t m = 0;
  if (web & WEB_MOD_SHIFT) m |= NEUI_KMOD_SHIFT;
  if (web & WEB_MOD_ALT)   m |= NEUI_KMOD_ALT;
  if (apple) {
    if (web & WEB_MOD_META) m |= NEUI_KMOD_CTRL;
    if (web & WEB_MOD_CTRL) m |= NEUI_KMOD_META;
  } else {
    if (web & WEB_MOD_CTRL) m |= NEUI_KMOD_CTRL;
    if (web & WEB_MOD_META) m |= NEUI_KMOD_META;
  }
  return m;
}

// The printable codepoint this press types, or 0: `key` is a single
// character ("Dead", "Enter", "Shift" ... are names, not characters), the
// primary modifier is not held (that is a shortcut, as on Linux / macOS),
// and it is not a control character.
inline uint32_t web_key_char(const char* key, uint32_t neui_mods)
{
  if (neui_mods & NEUI_KMOD_CTRL) return 0;
  const uint32_t cp = web_single_codepoint(key);
  if (cp < 0x20 || cp == 0x7F) return 0;
  return cp;
}

// Win32 MK_* bits for neui_event_mouse_t::buttonmap from PointerEvent.buttons
// (1 primary, 2 secondary, 4 middle) and the physical modifiers.
inline uint32_t web_buttonmap(uint32_t buttons, uint32_t web_mods)
{
  uint32_t m = 0;
  if (buttons & 1) m |= 0x0001;              // MK_LBUTTON
  if (buttons & 2) m |= 0x0002;              // MK_RBUTTON
  if (buttons & 4) m |= 0x0010;              // MK_MBUTTON
  if (web_mods & WEB_MOD_SHIFT) m |= 0x0004; // MK_SHIFT
  if (web_mods & WEB_MOD_CTRL)  m |= 0x0008; // MK_CONTROL
  return m;
}

} // namespace neui_detail
