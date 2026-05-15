#pragma once

#ifdef __APPLE__

#import <AppKit/AppKit.h>

#include "../theme_palette.h"

// macOS theme provider. Mirror of theme_provider_win32.h.
//
// Populates the process-wide neui_detail::Palette from NSAppearance + NSColor
// values, and installs distributed-notification observers so the palette
// refreshes live when the user toggles system Appearance / Accent Color in
// System Settings.
//
// CONVENTION (matches theme_provider_win32.h): include from exactly one
// translation unit (in this codebase, hosts/crossplatform/platform_macos.mm).
// The functions below are inline so multiple includes within that single TU
// are harmless, but the static state means cross-TU duplication would
// double-register listeners.

namespace neui_detail
{
  // Convert an NSColor to 0xAARRGGBB by resolving in sRGB space. Dynamic
  // colors (windowBackgroundColor etc.) must be resolved within the
  // current effective appearance for the right light/dark variant -
  // populate_palette_macos handles that via -performAsCurrentDrawingAppearance:.
  inline uint32_t nscolor_to_argb(NSColor* c)
  {
    if (!c) return 0;
    NSColor* s = [c colorUsingColorSpace:[NSColorSpace sRGBColorSpace]];
    if (!s) return 0;
    CGFloat r = 0, g = 0, b = 0, a = 1;
    [s getRed:&r green:&g blue:&b alpha:&a];
    auto byte = [](CGFloat v) -> uint32_t {
      if (v < 0) v = 0;
      if (v > 1) v = 1;
      return (uint32_t)(v * 255.0 + 0.5);
    };
    return (byte(a) << 24) | (byte(r) << 16) | (byte(g) << 8) | byte(b);
  }

  inline bool is_dark_appearance_macos()
  {
    NSAppearance* eff = NSApp.effectiveAppearance;
    if (!eff) return false;
    NSAppearanceName matched = [eff bestMatchFromAppearancesWithNames:@[
      NSAppearanceNameAqua, NSAppearanceNameDarkAqua
    ]];
    return [matched isEqualToString:NSAppearanceNameDarkAqua];
  }

  inline void populate_palette_macos(Palette& p)
  {
    bool dark = is_dark_appearance_macos();
    // Start from our hand-tuned defaults so any roles macOS doesn't expose
    // (control_bg_alt, scrollbar_*, ime_underline_*) get sensible values.
    p = dark ? default_dark_palette() : default_light_palette();
    p.is_dark = dark;

    // Resolve dynamic NSColors inside the current appearance so the right
    // light/dark variant lands in the palette. -performAsCurrentDrawingAppearance:
    // is macOS 11+; on older systems the resolved values may reflect the
    // wrong mode for some colors, but the default palette underneath
    // already gives a usable result.
    __block NSColor* accent  = nil;
    __block NSColor* win_bg  = nil;
    __block NSColor* ctrl_bg = nil;
    __block NSColor* text    = nil;
    __block NSColor* sep     = nil;
    void (^pull)(void) = ^{
      accent  = [NSColor controlAccentColor];
      win_bg  = [NSColor windowBackgroundColor];
      ctrl_bg = [NSColor controlBackgroundColor];
      text    = [NSColor textColor];
      sep     = [NSColor separatorColor];
    };
    if (@available(macOS 11.0, *)) {
      [NSApp.effectiveAppearance performAsCurrentDrawingAppearance:pull];
    } else {
      pull();
    }

    if (uint32_t v = nscolor_to_argb(accent); v) {
      p.colors[(size_t)ColorRole::accent] = v;
      // 50% translucent variant for selection highlights.
      p.colors[(size_t)ColorRole::accent_translucent] =
        (v & 0x00FFFFFFu) | 0x80000000u;
    }
    if (uint32_t v = nscolor_to_argb(win_bg);  v) {
      p.colors[(size_t)ColorRole::frame_bg] = v;
      p.colors[(size_t)ColorRole::panel_bg] = v;
    }
    if (uint32_t v = nscolor_to_argb(ctrl_bg); v)
      p.colors[(size_t)ColorRole::control_bg] = v;
    if (uint32_t v = nscolor_to_argb(text);    v)
      p.colors[(size_t)ColorRole::text_primary] = v;
    if (uint32_t v = nscolor_to_argb(sep);     v)
      p.colors[(size_t)ColorRole::border] = v;
  }

  inline void refresh_theme_palette_macos()
  {
    Palette next{};
    populate_palette_macos(next);
    Palette& cur = mutable_current_palette();
    next.version = cur.version + 1;
    cur = next;
    broadcast_theme_change();
  }

  // Idempotent. First call seeds the palette and installs the distributed
  // notification observers; subsequent calls are no-ops.
  inline void ensure_theme_provider_macos()
  {
    static bool initialised = false;
    if (initialised) return;
    initialised = true;

    refresh_theme_palette_macos();

    // Block-based observers - no @interface required in this header. Both
    // notifications fire from the cross-process distributed center when the
    // user toggles Appearance or Accent Color in System Settings.
    void (^handler)(NSNotification*) = ^(NSNotification* /*note*/) {
      refresh_theme_palette_macos();
    };
    NSDistributedNotificationCenter* center =
      NSDistributedNotificationCenter.defaultCenter;
    [center addObserverForName:@"AppleInterfaceThemeChangedNotification"
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:handler];
    [center addObserverForName:@"AppleColorPreferencesChangedNotification"
                        object:nil
                         queue:NSOperationQueue.mainQueue
                    usingBlock:handler];
  }

} // namespace neui_detail

#endif // __APPLE__
