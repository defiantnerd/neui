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
#include <string>

#pragma comment(lib, "Windowscodecs")

// Win32 icon loader shared between hosts. Tries LoadImageW for .ico files
// first (best quality - multi-resolution); falls back to WIC decoding for
// PNG/BMP/JPG/etc. Header-only / inline so both hosts can include it
// without ODR conflicts.

namespace neui_detail
{
  // Load an HICON from a file. Caller must DestroyIcon when done.
  // Returns nullptr on failure.
  inline HICON load_icon_from_file_w(const wchar_t* path)
  {
    if (!path || !*path) return nullptr;

    // 1) Native-icon fast path: handles .ico (multi-resolution containers).
    if (HICON h = (HICON)LoadImageW(nullptr, path, IMAGE_ICON,
                                     0, 0, LR_LOADFROMFILE | LR_DEFAULTSIZE))
      return h;

    // 2) WIC fallback: decode any common bitmap format and synthesise an
    //    HICON via CreateIconIndirect.
    IWICImagingFactory* factory = nullptr;
    HRESULT hr = CoCreateInstance(CLSID_WICImagingFactory, nullptr,
                                  CLSCTX_INPROC_SERVER,
                                  IID_PPV_ARGS(&factory));
    if (FAILED(hr) || !factory) return nullptr;

    IWICBitmapDecoder* decoder = nullptr;
    hr = factory->CreateDecoderFromFilename(path, nullptr,
                                             GENERIC_READ,
                                             WICDecodeMetadataCacheOnLoad,
                                             &decoder);
    if (FAILED(hr) || !decoder) { factory->Release(); return nullptr; }

    IWICBitmapFrameDecode* frame = nullptr;
    hr = decoder->GetFrame(0, &frame);
    if (FAILED(hr) || !frame) {
      decoder->Release(); factory->Release(); return nullptr;
    }

    IWICFormatConverter* conv = nullptr;
    hr = factory->CreateFormatConverter(&conv);
    if (FAILED(hr) || !conv) {
      frame->Release(); decoder->Release(); factory->Release();
      return nullptr;
    }

    // Use straight (non-premultiplied) BGRA so the V5 alpha-mask path on
    // CreateIconIndirect renders identically across taskbar / alt-tab /
    // titlebar contexts. Premultiplied input would darken edges where
    // GDI re-multiplies internally.
    hr = conv->Initialize(frame, GUID_WICPixelFormat32bppBGRA,
                           WICBitmapDitherTypeNone, nullptr, 0.0,
                           WICBitmapPaletteTypeMedianCut);
    if (FAILED(hr)) {
      conv->Release(); frame->Release();
      decoder->Release(); factory->Release();
      return nullptr;
    }

    UINT w = 0, h = 0;
    conv->GetSize(&w, &h);
    if (w == 0 || h == 0) {
      conv->Release(); frame->Release();
      decoder->Release(); factory->Release();
      return nullptr;
    }

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

    HDC screen = GetDC(nullptr);
    void* bits = nullptr;
    HBITMAP hbm_color = CreateDIBSection(screen,
                                          reinterpret_cast<BITMAPINFO*>(&bi),
                                          DIB_RGB_COLORS,
                                          &bits, nullptr, 0);
    ReleaseDC(nullptr, screen);
    if (!hbm_color || !bits) {
      if (hbm_color) DeleteObject(hbm_color);
      conv->Release(); frame->Release();
      decoder->Release(); factory->Release();
      return nullptr;
    }

    UINT stride = w * 4;
    UINT bytes  = stride * h;
    hr = conv->CopyPixels(nullptr, stride, bytes,
                           static_cast<BYTE*>(bits));
    if (FAILED(hr)) {
      DeleteObject(hbm_color);
      conv->Release(); frame->Release();
      decoder->Release(); factory->Release();
      return nullptr;
    }

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

    conv->Release();
    frame->Release();
    decoder->Release();
    factory->Release();
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
        neu = load_icon_from_file_w(w.c_str());
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
