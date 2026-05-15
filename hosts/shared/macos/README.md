# macOS shared host utilities

This directory mirrors `hosts/shared/win32/`. It exists for header-only,
inline-only macOS-specific helpers that both host static libraries (xpl
host today; a hypothetical native AppKit host tomorrow) can include
without ODR violations.

The macOS port session is expected to add at least:

- `clipboard_macos.h` - NSPasteboard helpers used by `platform_clipboard_*`
  in `hosts/crossplatform/platform_macos.mm`. Mirror of
  `clipboard_win32.h`.
- `theme_provider_macos.h` - populates the global `neui_detail::Palette`
  from `NSAppearance` (light/dark) + `NSColor.controlAccentColor`. KVO on
  `NSApp.effectiveAppearance` for live updates. Mirror of
  `theme_provider_win32.h`.
- `image_loader_macos.h` - load `.png` / `.jpg` / `.icns` to BGRA8
  premultiplied via `CGImageSource`. Mirror of `icon_win32.h`'s WIC path.

These are scaffolded inside `platform_macos.mm` as `TODO` comments today;
factor them into header-only files here once the implementation grows
beyond a few lines per concern.

Why mirror the Win32 layout? The xpl host already imports
`hosts/shared/win32/clipboard_win32.h` etc. through `platform_win32.cpp`.
Putting the macOS equivalents under `hosts/shared/macos/` and importing
them from `platform_macos.mm` keeps the same shape: each platform layer
owns its OS-specific helpers, the host body stays portable.
