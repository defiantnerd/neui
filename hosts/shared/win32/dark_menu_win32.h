#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>

// Native menu (HMENU) dark-mode support on Win10 1809+ / Win11.
//
// Microsoft never exposed a documented API to flip native menus to dark.
// All shipping apps that have themed native menus (Notepad, Settings,
// File Explorer, Windows Terminal, …) reach for a small set of
// undocumented exports in `uxtheme.dll`, identified by ordinal:
//
//   ord 104 - RefreshImmersiveColorPolicyState()
//   ord 133 - AllowDarkModeForWindow(HWND, bool)
//   ord 135 - SetPreferredAppMode(int)              (Win10 1903+)
//   ord 135 - AllowDarkModeForApp(bool)             (Win10 1809 only)
//   ord 136 - FlushMenuThemes()
//
// The ordinal numbers themselves have been stable since Win10 1809
// (released October 2018). The functions are loaded lazily on first use
// via GetProcAddress and silently no-op on systems where they're missing,
// so this header is safe to include and call unconditionally.

namespace neui_detail
{
  // Win10 1903+ values for SetPreferredAppMode.
  enum PreferredAppMode {
    PAM_Default     = 0,
    PAM_AllowDark   = 1,   // permit per-window AllowDarkModeForWindow to take effect
    PAM_ForceDark   = 2,   // every window gets dark menus regardless of attr
    PAM_ForceLight  = 3,
    PAM_Max         = 4
  };

  using fnAllowDarkModeForWindow           = bool (WINAPI *)(HWND, bool);
  using fnAllowDarkModeForApp              = bool (WINAPI *)(bool);
  using fnSetPreferredAppMode              = int  (WINAPI *)(int);
  using fnRefreshImmersiveColorPolicyState = void (WINAPI *)();
  using fnFlushMenuThemes                  = void (WINAPI *)();

  struct DarkMenuApis {
    bool                                loaded = false;
    fnAllowDarkModeForWindow            AllowDarkModeForWindow         = nullptr;
    fnSetPreferredAppMode               SetPreferredAppMode            = nullptr;
    fnAllowDarkModeForApp               AllowDarkModeForApp            = nullptr;
    fnRefreshImmersiveColorPolicyState  RefreshImmersiveColorPolicyState = nullptr;
    fnFlushMenuThemes                   FlushMenuThemes                = nullptr;
  };

  inline DarkMenuApis& dark_menu_apis()
  {
    static DarkMenuApis a;
    return a;
  }

  inline void load_dark_menu_apis()
  {
    auto& api = dark_menu_apis();
    if (api.loaded) return;
    api.loaded = true;
    HMODULE ux = LoadLibraryExW(L"uxtheme.dll", nullptr,
                                 LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!ux) return;
    // Ordinal 135 changed signature between 1809 and 1903. We try
    // SetPreferredAppMode first (the newer, richer signature); on 1809
    // the same address points at AllowDarkModeForApp(bool) - calling
    // either with a non-zero int argument enables dark, so the practical
    // effect is similar enough.
    api.SetPreferredAppMode =
      reinterpret_cast<fnSetPreferredAppMode>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    api.AllowDarkModeForApp =
      reinterpret_cast<fnAllowDarkModeForApp>(GetProcAddress(ux, MAKEINTRESOURCEA(135)));
    api.AllowDarkModeForWindow =
      reinterpret_cast<fnAllowDarkModeForWindow>(GetProcAddress(ux, MAKEINTRESOURCEA(133)));
    api.RefreshImmersiveColorPolicyState =
      reinterpret_cast<fnRefreshImmersiveColorPolicyState>(GetProcAddress(ux, MAKEINTRESOURCEA(104)));
    api.FlushMenuThemes =
      reinterpret_cast<fnFlushMenuThemes>(GetProcAddress(ux, MAKEINTRESOURCEA(136)));
  }

  // Process-wide app mode hint. AllowDark = "let per-window opt-in
  // decide". ForceLight = override per-window opt-in to keep menus
  // light. Call once after the system theme is known and again on every
  // theme change; cheap.
  //
  // Flushes cached menu theme handles via FlushMenuThemes - without
  // this, popups that were previously themed dark stay dark even after
  // a switch to light (and vice versa) because Windows holds onto the
  // last-resolved theme until something asks it to drop the cache.
  inline void set_app_dark_preference(bool dark)
  {
    load_dark_menu_apis();
    auto& api = dark_menu_apis();
    if (api.SetPreferredAppMode) {
      api.SetPreferredAppMode(dark ? PAM_AllowDark : PAM_ForceLight);
    } else if (api.AllowDarkModeForApp) {
      api.AllowDarkModeForApp(dark);
    }
    if (api.FlushMenuThemes)
      api.FlushMenuThemes();
    if (api.RefreshImmersiveColorPolicyState)
      api.RefreshImmersiveColorPolicyState();
  }

  // Per-frame opt-in. Pair with set_app_dark_preference + a
  // WM_THEMECHANGED to force the menu to repaint with the new mode.
  inline void apply_dark_window_mode(HWND hwnd, bool dark)
  {
    if (!hwnd) return;
    load_dark_menu_apis();
    auto& api = dark_menu_apis();
    if (api.AllowDarkModeForWindow)
      api.AllowDarkModeForWindow(hwnd, dark);
    // Trigger a theme refresh so the menubar repaints with the new mode.
    SendMessageW(hwnd, WM_THEMECHANGED, 0, 0);
  }

} // namespace neui_detail

#endif // _WIN32
