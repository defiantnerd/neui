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
#include <cstring>

// Win32 system-clipboard helpers. Header-only / `inline` so multiple host
// static libs can include them without ODR violations.

namespace neui_detail
{
  // True if the clipboard currently advertises Unicode text.
  inline bool clipboard_has_text_win32()
  {
    return IsClipboardFormatAvailable(CF_UNICODETEXT) != 0;
  }

  // Replace clipboard contents with UTF-8 text. Returns true on success.
  inline bool clipboard_set_text_win32(const char* utf8, uint32_t length)
  {
    if (!OpenClipboard(nullptr)) return false;

    bool ok = false;
    if (EmptyClipboard()) {
      // Convert UTF-8 to UTF-16 (no null terminator counted; we add one).
      int wn = 0;
      if (length > 0 && utf8) {
        wn = MultiByteToWideChar(CP_UTF8, 0, utf8,
                                 static_cast<int>(length), nullptr, 0);
      }
      // Allocate movable global memory for CF_UNICODETEXT (terminated).
      SIZE_T bytes = static_cast<SIZE_T>(wn + 1) * sizeof(wchar_t);
      HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, bytes);
      if (hg) {
        auto* p = static_cast<wchar_t*>(GlobalLock(hg));
        if (p) {
          if (wn > 0)
            MultiByteToWideChar(CP_UTF8, 0, utf8,
                                static_cast<int>(length), p, wn);
          p[wn] = L'\0';
          GlobalUnlock(hg);
          if (SetClipboardData(CF_UNICODETEXT, hg)) {
            ok = true;
            hg = nullptr;  // ownership passed to clipboard
          }
        }
        if (hg) GlobalFree(hg);
      }
    }

    CloseClipboard();
    return ok;
  }

  // Read clipboard text into buf as UTF-8. Returns total bytes needed
  // including null terminator. buf=NULL queries size only. Returns 0 if
  // clipboard has no text. Returns -1 on error.
  inline int clipboard_get_text_win32(char* buf, int buflen)
  {
    if (!IsClipboardFormatAvailable(CF_UNICODETEXT)) return 0;
    if (!OpenClipboard(nullptr)) return -1;

    int result = -1;
    HANDLE h = GetClipboardData(CF_UNICODETEXT);
    if (h) {
      auto* w = static_cast<const wchar_t*>(GlobalLock(h));
      if (w) {
        // Compute UTF-8 byte count INCLUDING the null terminator (passing -1
        // tells WideCharToMultiByte to include it).
        int needed = WideCharToMultiByte(CP_UTF8, 0, w, -1,
                                          nullptr, 0, nullptr, nullptr);
        if (needed > 0) {
          if (buf && buflen > 0) {
            int n = (buflen < needed) ? buflen : needed;
            WideCharToMultiByte(CP_UTF8, 0, w, -1, buf, n, nullptr, nullptr);
            // Force null termination if we truncated.
            if (n > 0) buf[n - 1] = '\0';
          }
          result = needed;
        }
        GlobalUnlock(h);
      }
    }

    CloseClipboard();
    return result;
  }

} // namespace neui_detail

#endif // _WIN32
