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
#include <vector>

// Singleton clipboard-change listener for Win32. A hidden HWND_MESSAGE window
// registers AddClipboardFormatListener; on each WM_CLIPBOARDUPDATE all
// registered (callback, token) pairs are invoked on the UI thread (the same
// thread that owns the listener window - ours is created lazily on first
// register from whichever thread that is, which in this codebase is the UI
// thread).
//
// Header-only (`inline static` storage) so both host static libs can include
// this without ODR violations.

namespace neui_detail
{
  using ClipboardListenerCallback = void (*)(void* token);

  struct ClipboardListenerEntry {
    ClipboardListenerCallback cb;
    void*                     token;
    uint32_t                  handle;
  };

  inline std::vector<ClipboardListenerEntry>& clipboard_listener_entries()
  {
    static std::vector<ClipboardListenerEntry> entries;
    return entries;
  }

  inline uint32_t& clipboard_listener_next_handle()
  {
    static uint32_t next = 1;
    return next;
  }

  inline HWND& clipboard_listener_hwnd()
  {
    static HWND hwnd = nullptr;
    return hwnd;
  }

  inline LRESULT CALLBACK clipboard_listener_proc(HWND hwnd, UINT msg,
                                                   WPARAM wp, LPARAM lp)
  {
    if (msg == WM_CLIPBOARDUPDATE) {
      // Snapshot the entry list so a callback that registers / unregisters
      // doesn't invalidate our iteration.
      auto snapshot = clipboard_listener_entries();
      for (auto& e : snapshot) {
        if (e.cb) e.cb(e.token);
      }
      return 0;
    }
    if (msg == WM_DESTROY) {
      RemoveClipboardFormatListener(hwnd);
      clipboard_listener_hwnd() = nullptr;
      return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
  }

  // Lazily register the window class and create the message-only window.
  inline HWND ensure_clipboard_listener_window()
  {
    HWND& hwnd = clipboard_listener_hwnd();
    if (hwnd) return hwnd;

    static const wchar_t* k_class = L"NeUiClipboardListener";
    static bool s_registered = false;

    HINSTANCE hi = GetModuleHandleW(nullptr);
    if (!s_registered) {
      WNDCLASSEXW wc = {};
      wc.cbSize        = sizeof(wc);
      wc.lpfnWndProc   = clipboard_listener_proc;
      wc.hInstance     = hi;
      wc.lpszClassName = k_class;
      // Ignore if already registered (shouldn't happen since s_registered
      // gates this, but RegisterClassExW returning 0 with ERROR_CLASS_ALREADY_EXISTS
      // is benign).
      RegisterClassExW(&wc);
      s_registered = true;
    }

    hwnd = CreateWindowExW(0, k_class, L"", 0, 0, 0, 0, 0,
                           HWND_MESSAGE, nullptr, hi, nullptr);
    if (hwnd) AddClipboardFormatListener(hwnd);
    return hwnd;
  }

  // Register a clipboard-change callback. Returns a non-zero handle on
  // success; 0 on failure. Pass the handle to unregister_clipboard_listener.
  inline uint32_t register_clipboard_listener(ClipboardListenerCallback cb,
                                               void* token)
  {
    if (!cb) return 0;
    if (!ensure_clipboard_listener_window()) return 0;
    uint32_t h = clipboard_listener_next_handle()++;
    clipboard_listener_entries().push_back({ cb, token, h });
    return h;
  }

  inline void unregister_clipboard_listener(uint32_t handle)
  {
    if (handle == 0) return;
    auto& v = clipboard_listener_entries();
    for (size_t i = 0; i < v.size(); ++i) {
      if (v[i].handle == handle) {
        v.erase(v.begin() + i);
        return;
      }
    }
  }

} // namespace neui_detail

#endif // _WIN32
