#pragma once

#include <cstdint>
#include <cstdio>
#include <string>
#include <neui/d/keys.h>

// Format a (modifiers, key) shortcut into a Win/Linux-style display label
// like "Ctrl+S", "Ctrl+Shift+Z", "F11", "Alt+F4". The label is what gets
// appended to a menu item's text (after a tab) so the OS can right-align
// it in the popup menu. format_shortcut_label_mac gives the Apple form
// (the modifier symbols, then the key; UTF-8) for a neui-painted menu on
// an Apple device (the macOS popup menu cascade, the web host in Safari),
// where NEUI_KMOD_CTRL is Cmd. format_shortcut_label picks the one the
// build platform's users expect.
//
// All return an empty string if `key` is NEUI_KEY_NONE.

namespace neui_detail
{
  inline const char* shortcut_key_name(uint32_t key)
  {
    switch (key) {
    case NEUI_KEY_BACK:   return "Backspace";
    case NEUI_KEY_TAB:    return "Tab";
    case NEUI_KEY_RETURN: return "Enter";
    case NEUI_KEY_ESCAPE: return "Esc";
    case NEUI_KEY_SPACE:  return "Space";
    case NEUI_KEY_END:    return "End";
    case NEUI_KEY_HOME:   return "Home";
    case NEUI_KEY_LEFT:   return "Left";
    case NEUI_KEY_UP:     return "Up";
    case NEUI_KEY_RIGHT:  return "Right";
    case NEUI_KEY_DOWN:   return "Down";
    case NEUI_KEY_INSERT: return "Insert";
    case NEUI_KEY_DELETE: return "Delete";
    case NEUI_KEY_F1:     return "F1";
    case NEUI_KEY_F2:     return "F2";
    case NEUI_KEY_F3:     return "F3";
    case NEUI_KEY_F4:     return "F4";
    case NEUI_KEY_F5:     return "F5";
    case NEUI_KEY_F6:     return "F6";
    case NEUI_KEY_F7:     return "F7";
    case NEUI_KEY_F8:     return "F8";
    case NEUI_KEY_F9:     return "F9";
    case NEUI_KEY_F10:    return "F10";
    case NEUI_KEY_F11:    return "F11";
    case NEUI_KEY_F12:    return "F12";
    default: return nullptr;
    }
  }

  inline std::string format_shortcut_label_win(uint32_t mods, uint32_t key)
  {
    if (key == NEUI_KEY_NONE) return {};

    std::string out;
    if (mods & NEUI_KMOD_CTRL)  out += "Ctrl+";
    if (mods & NEUI_KMOD_ALT)   out += "Alt+";
    if (mods & NEUI_KMOD_SHIFT) out += "Shift+";
    if (mods & NEUI_KMOD_META)  out += "Win+";

    if (const char* named = shortcut_key_name(key)) {
      out += named;
    } else if (key >= NEUI_KEY_0 && key <= NEUI_KEY_9) {
      out += static_cast<char>('0' + (key - NEUI_KEY_0));
    } else if (key >= NEUI_KEY_A && key <= NEUI_KEY_Z) {
      out += static_cast<char>('A' + (key - NEUI_KEY_A));
    } else {
      // Unknown key - drop the trailing '+' from the modifier prefix and
      // fall back to a hex display so the user sees something meaningful.
      if (!out.empty() && out.back() == '+') out.pop_back();
      char buf[16];
      std::snprintf(buf, sizeof(buf), "0x%02X", key);
      if (!out.empty()) out += '+';
      out += buf;
    }
    return out;
  }

  // Apple-style label: the modifier GLYPHS in Apple's fixed order (Control,
  // Option, Shift, Command - HIG "Modifier Keys"), no separators, then the key.
  //
  // neui's convention is that NEUI_KMOD_CTRL is the platform's PRIMARY command
  // modifier, so on Apple NEUI_KMOD_CTRL is Command and NEUI_KMOD_META the
  // physical Control key - the same mapping as mac_modifiers_to_neui (input)
  // and the native NSMenu keyEquivalent mask (menubar_macos.h). A shortcut
  // bound NEUI_KMOD_CTRL | NEUI_KEY_Z fires on Cmd+Z and reads "⌘Z" here, as it
  // does in the native menu bar.
  //
  // Why this exists: format_shortcut_label_win's output was never visible on
  // macOS before, because the native NSMenu path strips the "\tShortcut" suffix
  // (macos_menu_title_only) and shows keyEquivalent instead. Surfaces that DRAW
  // the label themselves - the NEUI_W_POPUPMENU cascade on macOS, a neui-painted
  // menu on an Apple device in the web host - must not read "Win+Z" there.
  inline std::string format_shortcut_label_mac(uint32_t mods, uint32_t key)
  {
    if (key == NEUI_KEY_NONE) return {};

    std::string out;
    if (mods & NEUI_KMOD_META)  out += "\xE2\x8C\x83";   // Control  U+2303
    if (mods & NEUI_KMOD_ALT)   out += "\xE2\x8C\xA5";   // Option   U+2325
    if (mods & NEUI_KMOD_SHIFT) out += "\xE2\x87\xA7";   // Shift    U+21E7
    if (mods & NEUI_KMOD_CTRL)  out += "\xE2\x8C\x98";   // Command  U+2318

    switch (key) {
    case NEUI_KEY_BACK:   out += "\xE2\x8C\xAB"; return out;   // U+232B
    case NEUI_KEY_DELETE: out += "\xE2\x8C\xA6"; return out;   // U+2326
    case NEUI_KEY_TAB:    out += "\xE2\x87\xA5"; return out;   // U+21E5
    case NEUI_KEY_RETURN: out += "\xE2\x86\xA9"; return out;   // U+21A9
    case NEUI_KEY_ESCAPE: out += "\xE2\x8E\x8B"; return out;   // U+238B
    case NEUI_KEY_LEFT:   out += "\xE2\x86\x90"; return out;   // U+2190
    case NEUI_KEY_UP:     out += "\xE2\x86\x91"; return out;   // U+2191
    case NEUI_KEY_RIGHT:  out += "\xE2\x86\x92"; return out;   // U+2192
    case NEUI_KEY_DOWN:   out += "\xE2\x86\x93"; return out;   // U+2193
    default: break;
    }

    if (const char* named = shortcut_key_name(key)) {
      out += named;
    } else if (key >= NEUI_KEY_0 && key <= NEUI_KEY_9) {
      out += static_cast<char>('0' + (key - NEUI_KEY_0));
    } else if (key >= NEUI_KEY_A && key <= NEUI_KEY_Z) {
      out += static_cast<char>('A' + (key - NEUI_KEY_A));
    } else {
      char buf[16];
      std::snprintf(buf, sizeof(buf), "0x%02X", key);
      out += buf;
    }
    return out;
  }

  // Pick the label style the running platform's users expect. Callers that must
  // have one specific style (a test, a cross-platform serialisation) should call
  // the concrete function instead. The web host decides at run time (the page
  // may be on an Apple device) and calls the concrete functions itself.
  inline std::string format_shortcut_label(uint32_t mods, uint32_t key)
  {
#if defined(__APPLE__)
    return format_shortcut_label_mac(mods, key);
#else
    return format_shortcut_label_win(mods, key);
#endif
  }

} // namespace neui_detail
