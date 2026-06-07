#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#include <shellapi.h>
#include <shlobj.h>
#include <cstdint>
#include <cstring>
#include <string>

#include "../clipboard_item.h"
#include "clipboard_format_html_win32.h"
#include "clipboard_format_urilist_win32.h"
#include "clipboard_format_png_win32.h"

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

  // -------------------------------------------------------------------------
  // Multi-format item read / write.

  // Make a movable HGLOBAL containing `len` bytes copied from `data`.
  inline HGLOBAL clipboard_make_global_bytes_win32(const void* data, size_t len)
  {
    HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, len > 0 ? len : 1);
    if (!hg) return nullptr;
    void* p = GlobalLock(hg);
    if (!p) { GlobalFree(hg); return nullptr; }
    if (data && len) std::memcpy(p, data, len);
    GlobalUnlock(hg);
    return hg;
  }

  // Place every format on `item` onto the system clipboard. Known MIMEs
  // (text/plain, text/html, text/uri-list) map to their native Windows
  // clipboard formats; everything else is published under its MIME string
  // as a RegisterClipboardFormatA name, so the same arbitrary blob can be
  // read back by another process that recognises the MIME.
  inline bool clipboard_write_item_win32(const DataItem& item)
  {
    if (!OpenClipboard(nullptr)) return false;
    if (!EmptyClipboard()) { CloseClipboard(); return false; }

    bool any = false;
    item.for_each_format([&](const std::string& mime,
                              const std::vector<uint8_t>& bytes) {
      // ---- text/plain -> CF_UNICODETEXT ----
      if (mime == "text/plain;charset=utf-8" || mime == "text/plain") {
        // Bytes may or may not include the trailing null; strip if present.
        uint32_t n = static_cast<uint32_t>(bytes.size());
        if (n > 0 && bytes[n - 1] == 0) --n;
        int wn = (n > 0)
          ? MultiByteToWideChar(CP_UTF8, 0,
                                reinterpret_cast<const char*>(bytes.data()),
                                static_cast<int>(n), nullptr, 0)
          : 0;
        SIZE_T sz = static_cast<SIZE_T>(wn + 1) * sizeof(wchar_t);
        HGLOBAL hg = GlobalAlloc(GMEM_MOVEABLE, sz);
        if (!hg) return;
        auto* w = static_cast<wchar_t*>(GlobalLock(hg));
        if (w) {
          if (wn > 0)
            MultiByteToWideChar(CP_UTF8, 0,
                                reinterpret_cast<const char*>(bytes.data()),
                                static_cast<int>(n), w, wn);
          w[wn] = L'\0';
          GlobalUnlock(hg);
          if (SetClipboardData(CF_UNICODETEXT, hg)) {
            any = true;
            return;
          }
        }
        GlobalFree(hg);
        return;
      }

      // ---- text/html -> CF_HTML (registered) ----
      if (mime == "text/html") {
        auto buf = clipboard_encode_cf_html(bytes.data(),
                                             static_cast<uint32_t>(bytes.size()));
        HGLOBAL hg = clipboard_make_global_bytes_win32(buf.data(), buf.size());
        if (!hg) return;
        if (SetClipboardData(clipboard_cf_html_format(), hg)) {
          any = true;
        } else {
          GlobalFree(hg);
        }
        return;
      }

      // ---- text/uri-list -> CF_HDROP ----
      if (mime == "text/uri-list") {
        HGLOBAL hg = urilist_to_hdrop_global(bytes.data(), bytes.size());
        if (!hg) return;
        if (SetClipboardData(CF_HDROP, hg)) {
          any = true;
        } else {
          GlobalFree(hg);
        }
        return;
      }

      // ---- image/png -> CF_DIBV5 (+ keep as registered MIME) ----
      // Publish both: native shells read CF_DIBV5; neui apps round-trip
      // the original PNG bytes verbatim through the registered MIME.
      if (mime == "image/png") {
        auto dib = png_bytes_to_dibv5_bytes_w32(
          bytes.data(), static_cast<uint32_t>(bytes.size()));
        if (!dib.empty()) {
          HGLOBAL hg = clipboard_make_global_bytes_win32(dib.data(), dib.size());
          if (hg) {
            if (SetClipboardData(CF_DIBV5, hg)) any = true;
            else                                GlobalFree(hg);
          }
        }
        // Fall through to also publish under the MIME name.
      }

      // ---- arbitrary MIME -> RegisterClipboardFormatA(mime) ----
      UINT cf = RegisterClipboardFormatA(mime.c_str());
      if (!cf) return;
      HGLOBAL hg = clipboard_make_global_bytes_win32(bytes.data(), bytes.size());
      if (!hg) return;
      if (SetClipboardData(cf, hg)) {
        any = true;
      } else {
        GlobalFree(hg);
      }
    });

    CloseClipboard();
    return any;
  }

  // Snapshot every known representation on the system clipboard into
  // `item`. Returns true if at least one format was captured.
  inline bool clipboard_read_item_win32(DataItem& item)
  {
    if (!OpenClipboard(nullptr)) return false;

    bool any = false;

    // ---- CF_UNICODETEXT -> text/plain ----
    if (HANDLE h = GetClipboardData(CF_UNICODETEXT)) {
      auto* w = static_cast<const wchar_t*>(GlobalLock(h));
      if (w) {
        int n = WideCharToMultiByte(CP_UTF8, 0, w, -1,
                                     nullptr, 0, nullptr, nullptr);
        if (n > 0) {
          std::vector<char> buf(static_cast<size_t>(n));
          WideCharToMultiByte(CP_UTF8, 0, w, -1, buf.data(), n,
                              nullptr, nullptr);
          item.set_format("text/plain;charset=utf-8",
                          buf.data(), static_cast<uint32_t>(n));
          any = true;
        }
        GlobalUnlock(h);
      }
    }

    // ---- CF_HTML -> text/html ----
    if (UINT cf_html = clipboard_cf_html_format()) {
      if (HANDLE h = GetClipboardData(cf_html)) {
        SIZE_T sz = GlobalSize(h);
        auto* p = GlobalLock(h);
        if (p && sz > 0) {
          auto fragment = clipboard_decode_cf_html(p, sz);
          if (!fragment.empty()) {
            item.set_format("text/html", fragment.data(),
                            static_cast<uint32_t>(fragment.size()));
            any = true;
          }
        }
        if (p) GlobalUnlock(h);
      }
    }

    // ---- CF_HDROP -> text/uri-list ----
    if (HANDLE h = GetClipboardData(CF_HDROP)) {
      auto bytes = urilist_from_hdrop(static_cast<HDROP>(h));
      if (!bytes.empty()) {
        item.set_format("text/uri-list", bytes.data(),
                        static_cast<uint32_t>(bytes.size()));
        any = true;
      }
    }

    // ---- CF_DIBV5 / CF_DIB -> image/png ----
    // Prefer CF_DIBV5 (more header info, alpha mask) and only fall back
    // to CF_DIB when V5 is absent. Skip if a previous registered MIME
    // pass already populated image/png (i.e. another neui app wrote it
    // verbatim, in which case the PNG bytes are higher fidelity than the
    // DIB->PNG re-encode would be).
    if (!item.has_format("image/png")) {
      HANDLE h = GetClipboardData(CF_DIBV5);
      if (!h) h = GetClipboardData(CF_DIB);
      if (h) {
        SIZE_T sz = GlobalSize(h);
        auto*  p  = GlobalLock(h);
        if (p && sz > 0) {
          auto png = dib_bytes_to_png_bytes_w32(
            static_cast<const uint8_t*>(p),
            static_cast<uint32_t>(sz));
          if (!png.empty()) {
            item.set_format("image/png", png.data(),
                            static_cast<uint32_t>(png.size()));
            any = true;
          }
        }
        if (p) GlobalUnlock(h);
      }
    }

    // ---- Enumerate registered formats whose name looks like a MIME
    //      (contains '/'), capture as opaque bytes under that MIME ----
    UINT fmt = 0;
    while ((fmt = EnumClipboardFormats(fmt)) != 0) {
      // Skip the standard formats we've already harvested.
      if (fmt == CF_UNICODETEXT || fmt == CF_TEXT || fmt == CF_OEMTEXT ||
          fmt == CF_HDROP) continue;
      if (clipboard_cf_html_format() && fmt == clipboard_cf_html_format()) continue;

      char name[256];
      int len = GetClipboardFormatNameA(fmt, name, sizeof(name));
      if (len <= 0) continue;
      std::string mime(name, name + len);
      // Heuristic for MIME-like names: must contain '/'.
      if (mime.find('/') == std::string::npos) continue;
      // Skip if we already stored it via a known mapping above.
      if (item.has_format(mime)) continue;

      if (HANDLE h = GetClipboardData(fmt)) {
        SIZE_T sz = GlobalSize(h);
        auto* p = GlobalLock(h);
        if (p && sz > 0) {
          item.set_format(mime, p, static_cast<uint32_t>(sz));
          any = true;
        }
        if (p) GlobalUnlock(h);
      }
    }

    CloseClipboard();
    return any;
  }

} // namespace neui_detail

#endif // _WIN32
