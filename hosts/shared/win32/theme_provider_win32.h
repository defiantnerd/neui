#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include <cstdint>

#include <winrt/windows.foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>

#include "../theme_palette.h"

// Win32 theme provider.
//
// Reads system theme via Windows.UI.ViewManagement.UISettings (UIColorType,
// UIElementType) and broadcasts changes to subscribers registered through
// theme_palette.h.
//
// `ColorValuesChanged` fires on a Windows Runtime thread pool thread, so
// the provider posts WM_APP_THEME_CHANGED to its hidden HWND_MESSAGE
// window and does the actual palette repopulation + listener dispatch on
// the UI thread (the thread that called ensure_theme_provider_win32()
// first - i.e. the UI thread that owns this process's message pump).
//
// Header-only `inline` storage, mirroring the clipboard listener pattern.

namespace neui_detail
{
  // ---- Palette population from UISettings --------------------------------

  inline uint32_t argb_from_winrt(const winrt::Windows::UI::Color& c)
  {
    return ((uint32_t)c.A << 24) | ((uint32_t)c.R << 16)
         | ((uint32_t)c.G <<  8) |  (uint32_t)c.B;
  }

  // The reliable Win10+ light/dark indicator. UISettings's UIColorType /
  // UIElementType return the high-contrast palette, which doesn't track
  // "Choose your default app mode" - so we read the registry value the
  // user actually toggles.
  inline bool query_apps_use_dark_mode()
  {
    HKEY key;
    if (RegOpenKeyExW(HKEY_CURRENT_USER,
          L"Software\\Microsoft\\Windows\\CurrentVersion\\Themes\\Personalize",
          0, KEY_READ, &key) != ERROR_SUCCESS) {
      return false;
    }
    DWORD value = 1;  // 1 = light, 0 = dark; default to light if missing
    DWORD size  = sizeof(value);
    DWORD type  = 0;
    LSTATUS r = RegQueryValueExW(key, L"AppsUseLightTheme", nullptr,
                                  &type,
                                  reinterpret_cast<LPBYTE>(&value), &size);
    RegCloseKey(key);
    if (r != ERROR_SUCCESS || type != REG_DWORD) return false;
    return value == 0;
  }

  // Build an opaque ARGB from a GetSysColor COLORREF. Returns 0 if the
  // call fails so callers can detect "not available".
  inline uint32_t argb_from_sys_color(int idx)
  {
    COLORREF c = GetSysColor(idx);
    return 0xFF000000u
         | ((uint32_t)GetRValue(c) << 16)
         | ((uint32_t)GetGValue(c) <<  8)
         |  (uint32_t)GetBValue(c);
  }

  // Populate `dst` with as much as the system tells us. Sources, in order:
  //   - AppsUseLightTheme (registry) - light/dark base mode (reliable)
  //   - UISettings::GetColorValue(UIColorType::*) - accent + variants (live)
  //   - UISettings::UIElementColor(UIElementType::*) - surface + text
  //     (primary system source, gated against the registry mode)
  //   - GetSysColor(COLOR_*) - borders + scrollbars (secondary, gated)
  // The "luminance sanity gate" prevents UIElementColor's classic /
  // high-contrast values from bleeding across modes: if the OS reports a
  // light Window surface but the registry says we're in dark mode (or vv),
  // those surface roles stay at the per-mode default. The accent overlay
  // is always safe and applies unconditionally.
  inline void populate_palette_from_uisettings(
      const winrt::Windows::UI::ViewManagement::UISettings& s,
      Palette& dst)
  {
    using winrt::Windows::UI::ViewManagement::UIColorType;
    using winrt::Windows::UI::ViewManagement::UIElementType;

    bool is_dark = query_apps_use_dark_mode();

    // Start from the per-mode default (the safety floor for any role we
    // can't pull from the OS or that fails the sanity gate).
    dst = is_dark ? default_dark_palette() : default_light_palette();
    dst.is_dark = is_dark;

    // ---- Accent (live from UISettings) ----------------------------------
    uint32_t accent     = argb_from_winrt(s.GetColorValue(UIColorType::Accent));
    uint32_t accent_d1  = argb_from_winrt(s.GetColorValue(UIColorType::AccentDark1));
    uint32_t accent_l2  = argb_from_winrt(s.GetColorValue(UIColorType::AccentLight2));
    if (accent & 0x00FFFFFF) {
      dst.colors[(size_t)ColorRole::accent]             = accent;
      dst.colors[(size_t)ColorRole::accent_translucent] = with_alpha(accent, 0x80);
      // Inactive selection: a desaturated accent reads better than a flat gray.
      dst.colors[(size_t)ColorRole::control_bg_inactive] =
        is_dark ? (accent_d1 | 0xFF000000) : (accent_l2 | 0xFF000000);
      // Default accent_text from luminance - overridden below by HighlightText
      // if UIElementColor returned something usable.
      dst.colors[(size_t)ColorRole::accent_text] =
        (luminance_argb(accent) > 180u) ? 0xFF000000 : 0xFFFFFFFF;
    }

    // ---- System surface + text from UIElementColor ----------------------
    uint32_t ui_window     = argb_from_winrt(s.UIElementColor(UIElementType::Window));
    uint32_t ui_windowtext = argb_from_winrt(s.UIElementColor(UIElementType::WindowText));
    uint32_t ui_btnface    = argb_from_winrt(s.UIElementColor(UIElementType::ButtonFace));
    uint32_t ui_graytext   = argb_from_winrt(s.UIElementColor(UIElementType::GrayText));
    uint32_t ui_highlight  = argb_from_winrt(s.UIElementColor(UIElementType::Highlight));
    uint32_t ui_hltext     = argb_from_winrt(s.UIElementColor(UIElementType::HighlightText));

    // GrayText + HighlightText work in either mode - disabled text is a
    // mid-gray, and Windows already picks contrasting highlight text.
    dst.colors[(size_t)ColorRole::text_disabled] = ui_graytext;
    if (ui_hltext & 0x00FFFFFF)
      dst.colors[(size_t)ColorRole::accent_text] = ui_hltext;
    if (!(accent & 0x00FFFFFF) && (ui_highlight & 0x00FFFFFF)) {
      // Fallback accent when UIColorType::Accent is unavailable.
      dst.colors[(size_t)ColorRole::accent]             = ui_highlight;
      dst.colors[(size_t)ColorRole::accent_translucent] = with_alpha(ui_highlight, 0x80);
    }

    // Sanity gate: only trust UIElementColor's surface roles when the
    // returned Window luminance matches the user's mode. If the values
    // came from the high-contrast / classic table, they'll lean light
    // even in dark mode (and the per-mode defaults are a better guess).
    uint32_t lum = luminance_argb(ui_window);
    bool surface_trustworthy = is_dark ? (lum < 96u) : (lum > 160u);

    if (surface_trustworthy) {
      // frame_bg / panel_bg deliberately stay at the per-mode default -
      // UIElementColor::ButtonFace returns the classic 0xF0F0F0 in light
      // mode, while modern Win11 apps render at 0xF3F3F3 (what
      // default_light_palette now matches). Only control surfaces and
      // text follow UIElementColor.
      (void)ui_btnface;
      dst.colors[(size_t)ColorRole::control_bg]      = ui_window;
      dst.colors[(size_t)ColorRole::control_bg_alt]  =
        is_dark ? shade(ui_window, +12) : shade(ui_window, -8);
      dst.colors[(size_t)ColorRole::text_primary]    = ui_windowtext;
      dst.colors[(size_t)ColorRole::border_focused]  = ui_windowtext;
      dst.colors[(size_t)ColorRole::text_secondary]  =
        is_dark ? shade(ui_windowtext, -64) : shade(ui_windowtext, +96);
      dst.colors[(size_t)ColorRole::focus_ring]      =
        is_dark ? shade(ui_windowtext, -64) : shade(ui_windowtext, +96);

      // GetSysColor borders + scrollbar track only when the surface gate
      // approved: those values come from the same classic table that we'd
      // otherwise distrust.
      uint32_t sc_3dshadow  = argb_from_sys_color(COLOR_3DSHADOW);
      uint32_t sc_3dlight   = argb_from_sys_color(COLOR_3DLIGHT);
      uint32_t sc_scrollbar = argb_from_sys_color(COLOR_SCROLLBAR);
      dst.colors[(size_t)ColorRole::border]              = sc_3dshadow;
      dst.colors[(size_t)ColorRole::scrollbar_track]     = sc_scrollbar;
      dst.colors[(size_t)ColorRole::scrollbar_thumb]     = sc_3dshadow;
      dst.colors[(size_t)ColorRole::scrollbar_separator] = sc_3dlight;
    }
    // If the gate rejected: per-mode defaults stay. accent / accent_text /
    // text_disabled overlays still apply (they're independent of the gate).
  }

  // ---- Listener window infrastructure ------------------------------------

  inline HWND& theme_listener_hwnd()
  {
    static HWND hwnd = nullptr;
    return hwnd;
  }

  inline winrt::Windows::UI::ViewManagement::UISettings& theme_uisettings()
  {
    static winrt::Windows::UI::ViewManagement::UISettings s{ nullptr };
    return s;
  }

  inline bool& theme_provider_initialised()
  {
    static bool b = false;
    return b;
  }

  // WM_APP-range message used to marshal ColorValuesChanged from a thread
  // pool thread back to the UI thread.
  static constexpr UINT k_wm_app_theme_changed = WM_APP + 0x0042;

  inline void refresh_theme_palette_win32()
  {
    auto& s = theme_uisettings();
    if (!s) return;
    Palette next{};
    try {
      populate_palette_from_uisettings(s, next);
    } catch (...) {
      return;  // leave previous palette intact on transient failure
    }
    Palette& cur = mutable_current_palette();
    next.version = cur.version + 1;
    cur = next;
    broadcast_theme_change();
  }

  inline LRESULT CALLBACK theme_listener_proc(HWND hwnd, UINT msg,
                                              WPARAM wp, LPARAM lp)
  {
    if (msg == k_wm_app_theme_changed) {
      refresh_theme_palette_win32();
      return 0;
    }
    if (msg == WM_SETTINGCHANGE) {
      // Belt-and-braces: ImmersiveColorSet fires when light/dark toggles
      // and predates ColorValuesChanged on older Win10 builds.
      const wchar_t* str = reinterpret_cast<const wchar_t*>(lp);
      if (str && wcscmp(str, L"ImmersiveColorSet") == 0) {
        refresh_theme_palette_win32();
      }
      return 0;
    }
    if (msg == WM_DESTROY) {
      theme_listener_hwnd() = nullptr;
      return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  inline HWND ensure_theme_listener_window()
  {
    HWND& hwnd = theme_listener_hwnd();
    if (hwnd) return hwnd;

    static const wchar_t* k_class = L"NeUiThemeListener";
    static bool s_registered = false;

    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!s_registered) {
      WNDCLASSEXW wc = {};
      wc.cbSize        = sizeof(wc);
      wc.lpfnWndProc   = theme_listener_proc;
      wc.hInstance     = hi;
      wc.lpszClassName = k_class;
      RegisterClassExW(&wc);
      s_registered = true;
    }

    hwnd = CreateWindowExW(0, k_class, L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, hi, nullptr);
    return hwnd;
  }

  // Idempotent. Call from each host's session-creation path; the first
  // call sets up the UISettings instance, the listener window, and the
  // ColorValuesChanged subscription. Subsequent calls do nothing.
  inline void ensure_theme_provider_win32()
  {
    if (theme_provider_initialised()) return;

    HWND hwnd = ensure_theme_listener_window();
    if (!hwnd) return;

    try {
      theme_uisettings() = winrt::Windows::UI::ViewManagement::UISettings{};
      auto& s = theme_uisettings();

      // Initial population.
      refresh_theme_palette_win32();

      // Subscribe. Handler fires on a WinRT thread pool thread; we
      // marshal back to the UI thread via PostMessage.
      s.ColorValuesChanged([](
        winrt::Windows::UI::ViewManagement::UISettings const& /*sender*/,
        winrt::Windows::Foundation::IInspectable const& /*args*/)
      {
        HWND h = theme_listener_hwnd();
        if (h) PostMessageW(h, k_wm_app_theme_changed, 0, 0);
      });
    } catch (...) {
      // UISettings construction can throw on very old Windows. Leave
      // the default palette in place; listeners still fire on
      // WM_SETTINGCHANGE.
    }

    theme_provider_initialised() = true;
  }

} // namespace neui_detail

#endif // _WIN32
