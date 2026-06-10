// Native Win32 toast overlay.
//
// Header-only helper used by the win32 native host (and trivially
// reusable by anyone else who wants a fly-in notification panel anchored
// to an HWND). Implements a borderless layered popup window painted via
// GDI - small enough that the D2D dependency would be overkill and big
// enough that GDI's per-pixel alpha story (a 32-bpp DIB + UpdateLayered-
// Window) gives us crisp text + smooth fade without flicker.
//
// Per-host feel (win32): the in-phase is short and snappy, the out-phase
// is a long slow fade with no movement - matches Win11 system toast feel.
// Hold time defaults to 2 seconds.
//
// Lifecycle:
//   neui_detail::toast_show_w32(parent_hwnd, "Hello\nWorld");
//
// The toast owns its HWND + per-tick state via GWLP_USERDATA and self-
// destructs when the lifetime elapses. Calling toast_show again on the
// same parent replaces any in-flight toast.
//
// ODR safety: every function inline. Multiple TUs may include this
// header. One per-process atom is registered on the first show.

#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <string>
#include <vector>

namespace neui_detail
{

  struct ToastW32State {
    HWND          parent_hwnd = nullptr;
    std::wstring  text_w;            // UTF-16 for DrawTextW
    int           lines        = 1;
    int           width_px     = 0;  // physical pixels
    int           height_px    = 0;
    int           dpi          = 96;
    int           line_h_px    = 0;
    int           pad_x_px     = 0;
    int           pad_y_px     = 0;
    DWORD         start_tick   = 0;
    DWORD         fade_in_ms   = 120;
    DWORD         hold_ms      = 2000;
    DWORD         fade_out_ms  = 600;
    UINT_PTR      timer_id     = 1;
    uint32_t      bg_color     = 0x00202020;  // BGR, applied via DIB
    uint32_t      fg_color     = 0x00FFFFFF;
    uint32_t      border_color = 0x00888888;
  };

  // Forward declarations.
  inline LRESULT CALLBACK toast_wndproc_w32(HWND, UINT, WPARAM, LPARAM);
  inline void toast_paint_w32(HWND hwnd, ToastW32State* st, BYTE alpha,
                               int top_y_px);
  inline void toast_tick_w32(HWND hwnd, ToastW32State* st);

  inline ATOM toast_register_class_w32()
  {
    static ATOM atom = 0;
    if (atom) return atom;
    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = toast_wndproc_w32;
    wc.hInstance     = GetModuleHandleW(nullptr);
    wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);
    wc.lpszClassName = L"NeuiToastW32";
    atom = RegisterClassExW(&wc);
    return atom;
  }

  inline std::wstring toast_utf8_to_wide(const char* utf8)
  {
    if (!utf8 || !*utf8) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring ws(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, ws.data(), n);
    return ws;
  }

  // Count '\n' separators + 1.
  inline int toast_count_lines(const std::wstring& s)
  {
    int n = 1;
    for (wchar_t c : s) if (c == L'\n') ++n;
    return n;
  }

  // 96-DPI logical px -> physical px for this monitor.
  inline int toast_log_to_phys(int logical, int dpi)
  {
    return MulDiv(logical, dpi, 96);
  }

  inline int toast_get_dpi_for(HWND hwnd)
  {
    typedef UINT(WINAPI* GetDpiForWindowFn)(HWND);
    static GetDpiForWindowFn p = reinterpret_cast<GetDpiForWindowFn>(
      GetProcAddress(GetModuleHandleW(L"user32"), "GetDpiForWindow"));
    if (p) return static_cast<int>(p(hwnd));
    HDC dc = GetDC(hwnd);
    int dpi = dc ? GetDeviceCaps(dc, LOGPIXELSY) : 96;
    if (dc) ReleaseDC(hwnd, dc);
    return dpi > 0 ? dpi : 96;
  }

  // Measure widest line in logical px using the supplied font.
  inline SIZE toast_measure_w32(HWND parent, const std::wstring& text_w,
                                  int dpi, HFONT hfont)
  {
    HDC dc = GetDC(parent);
    if (!dc) return { 0, 0 };
    HFONT old = (HFONT)SelectObject(dc, hfont);
    int max_w = 0;
    int line_h = 0;
    TEXTMETRICW tm = {};
    if (GetTextMetricsW(dc, &tm)) line_h = tm.tmHeight + tm.tmExternalLeading;

    size_t start = 0;
    while (start <= text_w.size()) {
      size_t end = text_w.find(L'\n', start);
      if (end == std::wstring::npos) end = text_w.size();
      SIZE sz = {};
      if (end > start) {
        GetTextExtentPoint32W(dc, text_w.data() + start, (int)(end - start), &sz);
      }
      if (sz.cx > max_w) max_w = sz.cx;
      if (end == text_w.size()) break;
      start = end + 1;
    }
    SelectObject(dc, old);
    ReleaseDC(parent, dc);
    (void)dpi;
    return { max_w, line_h };
  }

  inline HFONT toast_make_font_w32(int dpi)
  {
    LOGFONTW lf = {};
    lf.lfHeight  = -MulDiv(14, dpi, 96);  // ~14 logical px
    lf.lfWeight  = FW_NORMAL;
    lf.lfCharSet = DEFAULT_CHARSET;
    wcscpy_s(lf.lfFaceName, L"Segoe UI");
    return CreateFontIndirectW(&lf);
  }

  inline void toast_paint_w32(HWND hwnd, ToastW32State* st, BYTE alpha,
                                int top_y_px)
  {
    // Render a 32-bpp DIB premultiplied with `alpha`, then commit via
    // UpdateLayeredWindow.
    HDC screen = GetDC(nullptr);
    HDC mem    = CreateCompatibleDC(screen);

    BITMAPINFO bi = {};
    bi.bmiHeader.biSize        = sizeof(bi.bmiHeader);
    bi.bmiHeader.biWidth       = st->width_px;
    bi.bmiHeader.biHeight      = -st->height_px;  // top-down
    bi.bmiHeader.biPlanes      = 1;
    bi.bmiHeader.biBitCount    = 32;
    bi.bmiHeader.biCompression = BI_RGB;

    void* pixels = nullptr;
    HBITMAP dib  = CreateDIBSection(screen, &bi, DIB_RGB_COLORS, &pixels, nullptr, 0);
    HBITMAP old  = (HBITMAP)SelectObject(mem, dib);

    // Fill background (BGR layout, top byte = alpha; pre-multiplied).
    uint32_t* px = static_cast<uint32_t*>(pixels);
    uint32_t bg = st->bg_color;
    uint8_t br = (bg >> 0)  & 0xFF;
    uint8_t bgn= (bg >> 8)  & 0xFF;
    uint8_t bb = (bg >> 16) & 0xFF;
    uint32_t bg_pre = (uint32_t(alpha) << 24)
                    | (uint32_t((br * alpha) / 255) << 16)
                    | (uint32_t((bgn * alpha) / 255) << 8)
                    | (uint32_t((bb * alpha) / 255));
    for (int i = 0, n = st->width_px * st->height_px; i < n; ++i) px[i] = bg_pre;

    // 1px border
    uint32_t bc = st->border_color;
    uint8_t cr = (bc >> 0)  & 0xFF;
    uint8_t cg = (bc >> 8)  & 0xFF;
    uint8_t cb = (bc >> 16) & 0xFF;
    uint32_t bd_pre = (uint32_t(alpha) << 24)
                    | (uint32_t((cr * alpha) / 255) << 16)
                    | (uint32_t((cg * alpha) / 255) << 8)
                    | (uint32_t((cb * alpha) / 255));
    for (int x = 0; x < st->width_px; ++x) {
      px[x] = bd_pre;
      px[(st->height_px - 1) * st->width_px + x] = bd_pre;
    }
    for (int y = 0; y < st->height_px; ++y) {
      px[y * st->width_px] = bd_pre;
      px[y * st->width_px + (st->width_px - 1)] = bd_pre;
    }

    // Text. GDI text on the DIB does not produce pre-multiplied output
    // for the alpha channel, but we'll just live with that subtlety and
    // overwrite the corresponding alpha byte to `alpha` after DrawTextW
    // so the layered window blends the whole panel correctly.
    HFONT hfont = toast_make_font_w32(st->dpi);
    HFONT oldFont = (HFONT)SelectObject(mem, hfont);
    SetBkMode(mem, TRANSPARENT);
    SetTextColor(mem, RGB((st->fg_color >> 0) & 0xFF,
                            (st->fg_color >> 8) & 0xFF,
                            (st->fg_color >> 16) & 0xFF));

    int run_y = st->pad_y_px;
    size_t start = 0;
    while (start <= st->text_w.size()) {
      size_t end = st->text_w.find(L'\n', start);
      if (end == std::wstring::npos) end = st->text_w.size();
      RECT r = { st->pad_x_px, run_y,
                 st->width_px - st->pad_x_px, run_y + st->line_h_px };
      if (end > start) {
        DrawTextW(mem, st->text_w.data() + (int)start, (int)(end - start),
                   &r, DT_LEFT | DT_TOP | DT_NOPREFIX | DT_SINGLELINE);
      }
      run_y += st->line_h_px;
      if (end == st->text_w.size()) break;
      start = end + 1;
    }
    SelectObject(mem, oldFont);
    DeleteObject(hfont);

    // Restamp the alpha channel to the requested global alpha so
    // GDI-rendered text becomes visible through UpdateLayeredWindow
    // (DrawTextW leaves the alpha byte = 0 on the glyph pixels).
    for (int i = 0, n = st->width_px * st->height_px; i < n; ++i) {
      uint32_t v = px[i];
      uint8_t  a = (v >> 24) & 0xFF;
      if (a < alpha) {
        px[i] = (uint32_t(alpha) << 24) | (v & 0x00FFFFFF);
      }
    }

    // Anchor to parent client-area top + slide offset.
    RECT pcr;
    GetClientRect(st->parent_hwnd, &pcr);
    POINT topleft = { (pcr.right - pcr.left - st->width_px) / 2, top_y_px };
    ClientToScreen(st->parent_hwnd, &topleft);

    POINT src = { 0, 0 };
    SIZE sz   = { st->width_px, st->height_px };
    BLENDFUNCTION bf = { AC_SRC_OVER, 0, 255, AC_SRC_ALPHA };

    UpdateLayeredWindow(hwnd, screen, &topleft, &sz, mem, &src, 0,
                         &bf, ULW_ALPHA);

    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    ReleaseDC(nullptr, screen);
  }

  inline void toast_tick_w32(HWND hwnd, ToastW32State* st)
  {
    DWORD now = GetTickCount();
    DWORD elapsed = now - st->start_tick;
    DWORD total = st->fade_in_ms + st->hold_ms + st->fade_out_ms;
    if (elapsed >= total) {
      DestroyWindow(hwnd);
      return;
    }
    // Resting top-edge: `line_h` below the parent's client-area top.
    int rest_y  = st->line_h_px;
    int start_y = -st->height_px;
    BYTE alpha  = 255;
    int  top_y  = rest_y;
    if (elapsed < st->fade_in_ms) {
      // Fast snap-in: ease-out cubic. Win32 spec: appears fast.
      double p  = double(elapsed) / double(st->fade_in_ms);
      double ip = 1.0 - p;
      double e  = 1.0 - ip * ip * ip;
      alpha = static_cast<BYTE>(255.0 * e);
      top_y = static_cast<int>(double(start_y) +
              (double(rest_y) - double(start_y)) * e);
    } else if (elapsed < st->fade_in_ms + st->hold_ms) {
      alpha = 255;
      top_y = rest_y;
    } else {
      // Slow fade-out. Win32 spec: fades out slow, no slide (just alpha).
      DWORD oe = elapsed - st->fade_in_ms - st->hold_ms;
      double p = double(oe) / double(st->fade_out_ms);
      // Ease-in cubic.
      double e = p * p * p;
      alpha = static_cast<BYTE>(255.0 * (1.0 - e));
      top_y = rest_y;
    }
    toast_paint_w32(hwnd, st, alpha, top_y);
  }

  inline LRESULT CALLBACK toast_wndproc_w32(HWND hwnd, UINT msg,
                                              WPARAM wParam, LPARAM lParam)
  {
    if (msg == WM_NCCREATE) {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    auto* st = reinterpret_cast<ToastW32State*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
    switch (msg) {
      case WM_TIMER:
        if (st && wParam == st->timer_id) {
          toast_tick_w32(hwnd, st);
        }
        return 0;
      case WM_LBUTTONDOWN:
      case WM_RBUTTONDOWN:
      case WM_MBUTTONDOWN: {
        // Click on the toast: jump to the start of the fade-out phase so
        // the user gets an immediate visual dismiss. Re-project start_tick
        // so elapsed lands exactly at fade_in_ms + hold_ms.
        if (st) {
          DWORD now = GetTickCount();
          DWORD hold_end = st->fade_in_ms + st->hold_ms;
          st->start_tick = (now >= hold_end) ? (now - hold_end) : 0;
          toast_tick_w32(hwnd, st);
        }
        return 0;
      }
      case WM_DESTROY:
        if (st) {
          KillTimer(hwnd, st->timer_id);
          delete st;
          SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
        }
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  // Look up any existing toast attached to `parent` and destroy it so
  // the replacement-on-second-call contract holds.
  inline void toast_destroy_existing_w32(HWND parent)
  {
    HWND h = nullptr;
    while ((h = FindWindowExW(nullptr, h, L"NeuiToastW32", nullptr)) != nullptr) {
      if (GetWindow(h, GW_OWNER) == parent) {
        DestroyWindow(h);
        // FindWindowExW would skip the just-destroyed handle anyway;
        // walking once is enough since toast_show enforces one-at-a-
        // time per parent.
        break;
      }
    }
  }

  inline void toast_show_w32(HWND parent, const char* utf8_text)
  {
    if (!parent) return;
    toast_destroy_existing_w32(parent);
    toast_register_class_w32();

    auto* st = new ToastW32State();
    st->parent_hwnd = parent;
    st->text_w      = toast_utf8_to_wide(utf8_text);
    st->lines       = toast_count_lines(st->text_w);
    st->dpi         = toast_get_dpi_for(parent);
    st->pad_x_px    = toast_log_to_phys(18, st->dpi);
    st->pad_y_px    = toast_log_to_phys(12, st->dpi);
    HFONT hf = toast_make_font_w32(st->dpi);
    SIZE  ms = toast_measure_w32(parent, st->text_w, st->dpi, hf);
    DeleteObject(hf);

    if (ms.cy <= 0) ms.cy = toast_log_to_phys(20, st->dpi);
    st->line_h_px   = ms.cy;
    st->width_px    = ms.cx + 2 * st->pad_x_px;
    st->height_px   = ms.cy * st->lines + 2 * st->pad_y_px;

    // Clamp width to 70% of parent client.
    RECT pcr; GetClientRect(parent, &pcr);
    int max_w = static_cast<int>((pcr.right - pcr.left) * 0.7);
    if (st->width_px > max_w && max_w > 80) st->width_px = max_w;
    if (st->width_px < toast_log_to_phys(80, st->dpi))
      st->width_px = toast_log_to_phys(80, st->dpi);

    // Pick colours from current Win11 dark palette baseline (matches
    // the xpl host's control_bg_alt / border / text_primary roles).
    st->bg_color     = 0x002A2A2A;  // BGR(2A,2A,2A)
    st->fg_color     = 0x00FFFFFF;
    st->border_color = 0x00888888;

    HWND tw = CreateWindowExW(
      // No WS_EX_TRANSPARENT: the toast absorbs its own clicks so a click
      // on it triggers the fade-out dismiss path in toast_wndproc_w32.
      WS_EX_LAYERED | WS_EX_TOOLWINDOW | WS_EX_NOACTIVATE,
      L"NeuiToastW32",
      L"",
      WS_POPUP,
      0, 0, st->width_px, st->height_px,
      parent,                         // owner so the toast moves with the parent
      nullptr,
      GetModuleHandleW(nullptr),
      st);
    if (!tw) { delete st; return; }

    st->start_tick = GetTickCount();
    SetTimer(tw, st->timer_id, 16, nullptr);
    // Show without activating so input focus stays in the parent.
    ShowWindow(tw, SW_SHOWNOACTIVATE);
    // First paint immediately (the WM_TIMER tick is 16ms away).
    toast_tick_w32(tw, st);
  }

} // namespace neui_detail
