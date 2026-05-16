#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <wincodec.h>
#include <cstdint>
#include <cstring>
#include <string>

#include "image_loader_win32.h"

// Win32 icon loader shared between hosts. Tries LoadImageW for .ico files
// on disk first (best quality - multi-resolution containers); falls
// through to the shared image loader (resource-first then file) for
// PNG/BMP/JPG/etc. Header-only / inline so both hosts can include it
// without ODR conflicts.

namespace neui_detail
{
  // Build an HICON from a straight-alpha BGRA8 pixel buffer (`w` x `h`,
  // top-down, stride = w*4). Returns nullptr on failure. Caller is
  // responsible for the pixel buffer; we copy into a DIB section.
  inline HICON hicon_from_bgra8(uint32_t w, uint32_t h, const uint8_t* pixels)
  {
    if (!w || !h || !pixels) return nullptr;

    // 32bpp DIB section with explicit channel masks so the alpha channel is
    // honoured by GDI. Top-down (-h) so we can copy without flipping.
    BITMAPV5HEADER bi = {};
    bi.bV5Size        = sizeof(bi);
    bi.bV5Width       = static_cast<LONG>(w);
    bi.bV5Height      = -static_cast<LONG>(h);
    bi.bV5Planes      = 1;
    bi.bV5BitCount    = 32;
    bi.bV5Compression = BI_BITFIELDS;
    bi.bV5RedMask     = 0x00FF0000;
    bi.bV5GreenMask   = 0x0000FF00;
    bi.bV5BlueMask    = 0x000000FF;
    bi.bV5AlphaMask   = 0xFF000000;

    HDC   screen = GetDC(nullptr);
    void* bits   = nullptr;
    HBITMAP hbm_color = CreateDIBSection(screen,
                                          reinterpret_cast<BITMAPINFO*>(&bi),
                                          DIB_RGB_COLORS,
                                          &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!hbm_color || !bits) {
      if (hbm_color) DeleteObject(hbm_color);
      return nullptr;
    }

    memcpy(bits, pixels, static_cast<size_t>(w) * h * 4);

    // Empty 1-bit mask - alpha channel in the color bitmap drives transparency.
    HBITMAP hbm_mask = CreateBitmap(static_cast<int>(w), static_cast<int>(h),
                                     1, 1, nullptr);

    ICONINFO ii = {};
    ii.fIcon    = TRUE;
    ii.hbmColor = hbm_color;
    ii.hbmMask  = hbm_mask;
    HICON hicon = CreateIconIndirect(&ii);

    if (hbm_mask)  DeleteObject(hbm_mask);
    if (hbm_color) DeleteObject(hbm_color);
    return hicon;
  }

  // Load an HICON by name. Tries (in order):
  //   1. .ico on disk via LoadImageW + LR_LOADFROMFILE - preserves the
  //      multi-resolution container format only this path can read.
  //   2. Embedded standard ICON resource (RT_GROUP_ICON / RT_ICON, what
  //      rc.exe produces from `name ICON "file.ico"`) via LoadImageW on
  //      the EXE module - also multi-resolution. The name is wrapped in
  //      literal quotes to match the rc.exe quirk where quoted name
  //      strings are stored with the '"' characters baked in.
  //   3. Embedded user-defined "PNG" resource via the shared image
  //      loader, then synthesise HICON (single-resolution).
  //   4. Other bitmap formats on disk via the shared image loader.
  // Caller must DestroyIcon when done. Returns nullptr on failure.
  inline HICON load_icon_from_file_w(const wchar_t* wpath, const char* utf8_path)
  {
    if (wpath && *wpath) {
      if (HICON h = (HICON)LoadImageW(nullptr, wpath, IMAGE_ICON,
                                       0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE))
        return h;

      if (HMODULE hMod = GetModuleHandleW(nullptr)) {
        std::wstring res_name = L"\"" + std::wstring(wpath) + L"\"";
        if (HICON h = (HICON)LoadImageW(hMod, res_name.c_str(), IMAGE_ICON,
                                         0, 0, LR_DEFAULTSIZE))
          return h;
      }
    }

    // Straight (non-premultiplied) BGRA - the BITMAPV5 mask path on
    // CreateIconIndirect would otherwise darken edges where GDI
    // re-multiplies premultiplied input internally.
    uint32_t w = 0, h = 0;
    uint8_t* pixels = load_image_bgra8_w32(utf8_path, &w, &h,
                                            GUID_WICPixelFormat32bppBGRA);
    if (!pixels) return nullptr;
    HICON hicon = hicon_from_bgra8(w, h, pixels);
    free_image_bgra8_w32(pixels);
    return hicon;
  }

  // Apply an icon (UTF-8 path) to a window. Replaces any previous icon
  // tracked in `*owned` and updates `*owned` to the new HICON (nullptr if
  // the path is empty or load failed). Sends WM_SETICON for both
  // ICON_SMALL and ICON_BIG so taskbar / titlebar / alt-tab all pick it up.
  inline void apply_window_icon(HWND hwnd, const char* utf8_path, HICON* owned)
  {
    if (!hwnd) return;
    HICON neu = nullptr;
    if (utf8_path && *utf8_path) {
      int n = MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, nullptr, 0);
      if (n > 0) {
        std::wstring w(static_cast<size_t>(n - 1), L'\0');
        MultiByteToWideChar(CP_UTF8, 0, utf8_path, -1, &w[0], n);
        neu = load_icon_from_file_w(w.c_str(), utf8_path);
      }
    }

    HICON old = owned ? *owned : nullptr;
    if (owned) *owned = neu;

    SendMessageW(hwnd, WM_SETICON, ICON_SMALL, reinterpret_cast<LPARAM>(neu));
    SendMessageW(hwnd, WM_SETICON, ICON_BIG,   reinterpret_cast<LPARAM>(neu));

    if (old) DestroyIcon(old);
  }

  inline void destroy_window_icon(HICON* owned)
  {
    if (owned && *owned) {
      DestroyIcon(*owned);
      *owned = nullptr;
    }
  }

} // namespace neui_detail

#endif // _WIN32
