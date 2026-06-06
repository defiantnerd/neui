// Win32 platform layer for the crossplatform host.
// Implements native APPWINDOW and PLUGWINDOW using CreateWindowExW.
// WM_PAINT dispatches into the D2D rendering backend via Session::paint_frame.

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <imm.h>
#include <string>

#include "host.h"
#include "platform.h"
#include "../../backends/d2d/d2d_backend.h"
#include "../shared/win32/image_loader_win32.h"
#include "../shared/win32/icon_win32.h"
#include "../shared/win32/theme_provider_win32.h"
#include "../shared/theme_palette.h"
#include "../shared/win32/theme_brushes_win32.h"
#include "../shared/win32/dark_menu_win32.h"
#include "../shared/win32/dark_menubar_win32.h"
#include "../shared/win32/clipboard_win32.h"
#include "../shared/win32/dnd_target_win32.h"
#include "../shared/win32/dnd_source_win32.h"
#include <dwmapi.h>
#pragma comment(lib, "dwmapi.lib")

#pragma comment(lib, "imm32")

namespace xpl_host
{
  extern std::vector<std::unique_ptr<Session>> sessions;

  // -------------------------------------------------------------------------
  // Window class

  static const wchar_t* k_wndclass      = L"NeUiXplHost";
  static HINSTANCE      g_hinstance     = nullptr;
  static bool           g_class_registered = false;

  // Per-HWND data stored in GWLP_USERDATA.
  struct WindowUserData
  {
    Session* session;
    uint32_t widget_index;
    bool     tracking_mouse    = false;  // true while TrackMouseEvent is active
    WCHAR    pending_surrogate = 0;      // high surrogate waiting for its low pair
    // SMOOTH-scroll grid bounce timer: when a grid overscrolls in SMOOTH mode
    // we SetTimer the FRAME's HWND (grids on xpl have no HWND of their own)
    // and track which grid is bouncing so the WM_TIMER tick can find it.
    // Only one grid per frame is allowed to bounce at a time; a second
    // overscrolling grid replaces the first (rare edge case).
    uint32_t bouncing_grid_index = 0;
  };

  // Timer ID for the grid spring-back animation on the frame's HWND.
  static constexpr UINT_PTR XPL_GRID_BOUNCE_TIMER_ID = 0x6E78676B;  // 'nxgk'

  // UTF-8 -> UTF-16 helper used by menubar platform functions.
  static std::wstring to_wide(const char* utf8)
  {
    if (!utf8 || !*utf8) return {};
    int n = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (n <= 1) return {};
    std::wstring ws(n - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &ws[0], n);
    return ws;
  }

  // Extract signed mouse coordinates from a WM_MOUSE* lParam.
  // The cast to short is required for negative positions (multi-monitor).
  static inline int mouse_x(LPARAM lp) { return static_cast<int>(static_cast<short>(LOWORD(lp))); }
  static inline int mouse_y(LPARAM lp) { return static_cast<int>(static_cast<short>(HIWORD(lp))); }

  // Convert a physical pixel coordinate to logical (96 DPI baseline).
  static inline float phys_to_log(int phys, uint32_t dpi)
  {
    return static_cast<float>(phys) * 96.0f / static_cast<float>(dpi);
  }

  static WindowUserData* get_wud(HWND hwnd)
  {
    return reinterpret_cast<WindowUserData*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }

  // Count of live APPWINDOW instances; PostQuitMessage when it reaches 0.
  static int g_appwindow_count = 0;

  // -------------------------------------------------------------------------
  // Keyboard helpers

  // Build a modifier bitmask from current key state.
  // Bit layout matches a simple convention (not yet exposed in the public API):
  //   bit 0 = Shift, bit 1 = Ctrl, bit 2 = Alt/Menu
  static uint32_t build_modifiers()
  {
    uint32_t m = 0;
    if (GetKeyState(VK_SHIFT)   & 0x8000) m |= 1;
    if (GetKeyState(VK_CONTROL) & 0x8000) m |= 2;
    if (GetKeyState(VK_MENU)    & 0x8000) m |= 4;
    return m;
  }

  // Dispatch a key event: client gets first chance; if it returns false the
  // focused widget's virtual handler is called as fallback.
  static void dispatch_key_to_focused(Session* sess,
                                       neui_event_type_t type,
                                       uint32_t keycode)
  {
    if (!sess) return;
    uint32_t fw = sess->_focused_widget;
    if (fw == 0 || !sess->_widgets.exists(fw)) return;
    auto& wd = sess->_widgets[fw];
    bool consumed = false;
    if (wd.emit_events) {
      neui_event_t ev = {};
      ev.type     = type;
      ev.data.key = { { wd.widget_id }, keycode, build_modifiers() };
      consumed = sess->dispatch_event(&ev);
    }
    if (!consumed)
      sess->handle_input_key(type, keycode, build_modifiers());
  }

  // -------------------------------------------------------------------------
  // IME helpers
  //
  // Composition messages arrive on the frame HWND (the only HWND in this
  // host). We resolve the logically-focused widget from Session and forward
  // composition events to its WidgetData::on_composition virtual. Widgets
  // that cannot accept composition (buttons, lists, ...) return false from
  // the default and the message falls through to DefWindowProcW.

  static WidgetData* focused_text_widget(Session* sess)
  {
    if (!sess) return nullptr;
    uint32_t fw = sess->_focused_widget;
    if (fw == 0 || !sess->_widgets.exists(fw)) return nullptr;
    return &sess->_widgets[fw];
  }

  // UTF-16 -> UTF-8. Returns empty on empty input.
  static std::string utf16_to_utf8(const wchar_t* w, int wlen)
  {
    if (!w || wlen <= 0) return {};
    int n = WideCharToMultiByte(CP_UTF8, 0, w, wlen, nullptr, 0, nullptr, nullptr);
    if (n <= 0) return {};
    std::string out(static_cast<size_t>(n), '\0');
    WideCharToMultiByte(CP_UTF8, 0, w, wlen, &out[0], n, nullptr, nullptr);
    return out;
  }

  // Translate a caret offset given in UTF-16 code units into a UTF-8 byte
  // offset for the same prefix of the composition string.
  static int utf16_caret_to_utf8(const wchar_t* w, int wlen, int caret_w)
  {
    if (!w || wlen <= 0 || caret_w <= 0) return 0;
    if (caret_w > wlen) caret_w = wlen;
    int n = WideCharToMultiByte(CP_UTF8, 0, w, caret_w,
                                 nullptr, 0, nullptr, nullptr);
    return n > 0 ? n : 0;
  }

  // Translate a GCS_COMPATTR array (one byte per UTF-16 code unit, ATTR_*
  // values) into a parallel array sized to the UTF-8 string. Each UTF-16
  // codepoint's attribute is repeated for every UTF-8 byte the codepoint
  // occupies. Surrogate pair halves carry the same attribute (the IME
  // gives the same value for both halves anyway). Maps Win32 ATTR_*
  // values to WidgetData::CompAttr.
  //
  // Returns the populated byte vector. Empty on bad input.
  static std::vector<uint8_t> compattr_to_utf8(const wchar_t* w, int wlen,
                                                const uint8_t* w_attrs)
  {
    std::vector<uint8_t> out;
    if (!w || wlen <= 0 || !w_attrs) return out;

    auto map_attr = [](uint8_t imm_attr) -> uint8_t {
      // Constants from <imm.h>:
      //   ATTR_INPUT               = 0
      //   ATTR_TARGET_CONVERTED    = 1
      //   ATTR_CONVERTED           = 2
      //   ATTR_TARGET_NOTCONVERTED = 3
      //   ATTR_INPUT_ERROR         = 4
      //   ATTR_FIXEDCONVERTED      = 5  -> treat as CONVERTED
      switch (imm_attr) {
      case ATTR_INPUT:               return 0;
      case ATTR_TARGET_CONVERTED:    return 1;
      case ATTR_CONVERTED:           return 2;
      case ATTR_TARGET_NOTCONVERTED: return 3;
      case ATTR_INPUT_ERROR:         return 4;
      case ATTR_FIXEDCONVERTED:      return 2;
      default:                       return 0;
      }
    };

    // Walk codepoints. A high surrogate combines with the next low surrogate
    // into one codepoint; otherwise each WCHAR is its own codepoint.
    for (int i = 0; i < wlen; ) {
      int unit_count = 1;
      if (i + 1 < wlen &&
          w[i]   >= 0xD800 && w[i]   <= 0xDBFF &&
          w[i+1] >= 0xDC00 && w[i+1] <= 0xDFFF) {
        unit_count = 2;
      }
      // Bytes this codepoint occupies in UTF-8.
      int byte_count = WideCharToMultiByte(CP_UTF8, 0, w + i, unit_count,
                                            nullptr, 0, nullptr, nullptr);
      uint8_t attr = map_attr(w_attrs[i]);
      for (int b = 0; b < byte_count; ++b) out.push_back(attr);
      i += unit_count;
    }
    return out;
  }

  // Position the IME composition / candidate window at the caret of the
  // currently focused text widget. The IMM rect is in client-pixel
  // coordinates of the frame HWND. caret_rect_local returns logical pixels
  // relative to the widget; we add the widget's logical (x, y), scale by the
  // frame DPI, and hand it to ImmSetCompositionWindow.
  static void set_composition_window_at_caret(HWND hwnd,
                                               Session* sess,
                                               WidgetData* wd,
                                               WidgetData* frame_wd)
  {
    if (!hwnd || !wd || !frame_wd) return;
    auto* backend = platform_get_backend();
    if (!backend) return;

    float lx = 0.0f, ly = 0.0f, lh = 0.0f;
    if (!wd->caret_rect_local(backend, frame_wd->render_ctx, &lx, &ly, &lh))
      return;

    // Widget logical x/y -> frame-client logical px -> physical px via DPI.
    // The widget's stored x/y is parent-relative; abs_x / abs_y is the
    // cached frame-local position (recomputed during the paint walk).
    float lx_in_frame = lx + static_cast<float>(wd->abs_x);
    float ly_in_frame = ly + static_cast<float>(wd->abs_y);
    UINT  dpi          = frame_wd->dpi ? frame_wd->dpi : 96;
    int   px_x         = MulDiv(static_cast<int>(lx_in_frame), static_cast<int>(dpi), 96);
    int   px_y         = MulDiv(static_cast<int>(ly_in_frame), static_cast<int>(dpi), 96);
    int   px_h         = MulDiv(static_cast<int>(lh),          static_cast<int>(dpi), 96);

    HIMC himc = ImmGetContext(hwnd);
    if (!himc) return;

    COMPOSITIONFORM cf = {};
    cf.dwStyle      = CFS_RECT;
    cf.ptCurrentPos = { px_x, px_y };
    cf.rcArea.left   = px_x;
    cf.rcArea.top    = px_y;
    cf.rcArea.right  = px_x + 1;
    cf.rcArea.bottom = px_y + px_h;
    ImmSetCompositionWindow(himc, &cf);

    CANDIDATEFORM caf = {};
    caf.dwIndex     = 0;
    caf.dwStyle     = CFS_EXCLUDE;
    caf.ptCurrentPos = { px_x, px_y };
    caf.rcArea.left   = px_x;
    caf.rcArea.top    = px_y;
    caf.rcArea.right  = px_x + 1;
    caf.rcArea.bottom = px_y + px_h;
    ImmSetCandidateWindow(himc, &caf);

    ImmReleaseContext(hwnd, himc);
    (void)sess;
  }

  // -------------------------------------------------------------------------
  // Window procedure

  static LRESULT CALLBACK XplWndProc(HWND hwnd, UINT msg,
                                      WPARAM wParam, LPARAM lParam)
  {
    // Native menu-bar dark-mode owner-draw. The owner-draw helper reads
    // current_palette(), so scope the override to this session's
    // effective palette first (multi-session correctness).
    if (msg == neui_detail::k_wm_uah_drawmenu ||
        msg == neui_detail::k_wm_uah_drawmenuitem) {
      auto* wud = get_wud(hwnd);
      neui_detail::ScopedPaletteOverride scope(
        (wud && wud->session) ? wud->session->effective_palette_ptr() : nullptr);
      LRESULT r = 0;
      if (neui_detail::handle_uah_menubar_message(hwnd, msg, wParam, lParam, r))
        return r;
    }

    switch (msg)
    {
    case WM_NCCREATE: {
      auto* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA,
        reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }

    case WM_CREATE: {
      auto* wud = get_wud(hwnd);
      if (wud) {
        auto* wd = wud->session->get_widget(wud->widget_index);
        if (wd && wd->type && !strcmp(wd->type, NEUI_W_APPWINDOW))
          ++g_appwindow_count;
      }
      // Give this window Win32 keyboard focus so WM_KEYDOWN/WM_CHAR arrive here.
      SetFocus(hwnd);
      return 0;
    }

    case WM_SETTINGCHANGE: {
      // Windows broadcasts WM_SETTINGCHANGE to top-level windows when the
      // user toggles light/dark mode (lParam = "ImmersiveColorSet") or
      // changes any other system setting. Refresh the palette + broadcast
      // - UISettings::ColorValuesChanged is unreliable in desktop Win32
      // apps without a CoreApplication / DispatcherQueue, so we use this
      // as the primary signal.
      const wchar_t* str = reinterpret_cast<const wchar_t*>(lParam);
      if (!str || wcscmp(str, L"ImmersiveColorSet") == 0) {
        neui_detail::refresh_theme_palette_win32();
      }
      return 0;
    }
    case WM_SYSCOLORCHANGE: {
      // Classic colour scheme changed - pull fresh GetSysColor-sourced
      // border / scrollbar values.
      neui_detail::refresh_theme_palette_win32();
      return 0;
    }

    case WM_CLOSE: {
      auto* wud = get_wud(hwnd);
      if (wud) {
        neui_event_t ev = {};
        ev.type = NEUI_EVENT_APP_QUIT;
        bool allow = wud->session->dispatch_event(&ev);
        if (allow)
          DestroyWindow(hwnd);
      }
      return 0;
    }

    case WM_DESTROY: {
      auto* wud = get_wud(hwnd);
      if (wud) {
        // Release the D2D render context before the HWND is gone.
        auto* wd = wud->session->get_widget(wud->widget_index);
        if (wd) {
          auto* backend = platform_get_backend();
          if (backend && wd->render_ctx) {
            wud->session->_asset_manager.release_context(wd->render_ctx, backend);
            backend->destroy_context(wd->render_ctx);
            wd->render_ctx = nullptr;
          }
          // Decrement appwindow count; quit message loop when last one closes.
          // Dialogs do NOT participate in the quit count.
          if (wd->type && !strcmp(wd->type, NEUI_W_APPWINDOW)) {
            if (--g_appwindow_count <= 0)
              PostQuitMessage(0);
          }
          // Auto-close any frames owned by this one. Clear our own
          // native_handle first so the owned dialogs' teardown skips the
          // "re-enable owner" path. DestroyWindow re-enters WM_DESTROY
          // synchronously for each owned frame.
          {
            uint32_t self_idx = wud->widget_index;
            wd->native_handle = nullptr;
            for (uint32_t idx : wud->session->_widgets.release_order()) {
              if (idx == 0 || idx == self_idx) continue;
              if (!wud->session->_widgets.exists(idx)) continue;
              auto& other = wud->session->_widgets[idx];
              if (other.owner_index == self_idx && other.native_handle) {
                HWND child_hwnd = static_cast<HWND>(other.native_handle);
                other.native_handle = nullptr;
                DestroyWindow(child_hwnd);
              }
            }
          }
          // Dialog teardown: re-enable and re-activate the owner so input
          // returns there. Must happen before native_handle is cleared, since
          // owner_index is read from this widget's state. Skip when the
          // dialog opted into modeless (NEUI_ATTR_MODAL = 0) - the owner
          // was never disabled and we shouldn't steal its activation.
          if (wd->type && !strcmp(wd->type, NEUI_W_DIALOG) &&
              wd->owner_index != 0 &&
              wud->session->_widgets.exists(wd->owner_index)) {
            bool is_modal = !wd->attrs ||
                            wd->attrs->get_int(NEUI_ATTR_MODAL, 1) != 0;
            void* owner_native = wud->session->_widgets[wd->owner_index].native_handle;
            if (is_modal && owner_native) {
              EnableWindow(static_cast<HWND>(owner_native), TRUE);
              SetForegroundWindow(static_cast<HWND>(owner_native));
            }
          }
          wd->native_handle = nullptr;
        }
      }
      return 0;
    }

    case WM_NCDESTROY: {
      // Free WindowUserData allocated in create_appwindow / create_plugwindow.
      auto* wud = get_wud(hwnd);
      SetWindowLongPtrW(hwnd, GWLP_USERDATA, 0);
      delete wud;
      return 0;
    }

    case WM_ERASEBKGND:
      // D2D handles background; suppress default GDI erase.
      return 1;

    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      auto* wud = get_wud(hwnd);
      if (wud) {
        auto* wd = wud->session->get_widget(wud->widget_index);
        if (wd && wd->render_ctx)
          wud->session->paint_frame(wd->render_ctx, wud->widget_index);
      }
      return 0;
    }

    case WM_GETMINMAXINFO: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      auto* fwd = wud->session->get_widget(wud->widget_index);
      if (!fwd || !fwd->attrs) break;

      MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
      UINT dpi = fwd->dpi ? fwd->dpi : 96;

      int min_w = fwd->attrs->get_int(NEUI_ATTR_MIN_WIDTH,  0);
      int min_h = fwd->attrs->get_int(NEUI_ATTR_MIN_HEIGHT, 0);
      int max_w = fwd->attrs->get_int(NEUI_ATTR_MAX_WIDTH,  0);
      int max_h = fwd->attrs->get_int(NEUI_ATTR_MAX_HEIGHT, 0);

      if (min_w > 0) mmi->ptMinTrackSize.x = MulDiv(min_w, static_cast<int>(dpi), 96);
      if (min_h > 0) mmi->ptMinTrackSize.y = MulDiv(min_h, static_cast<int>(dpi), 96);
      if (max_w > 0 && (min_w <= 0 || max_w >= min_w))
        mmi->ptMaxTrackSize.x = MulDiv(max_w, static_cast<int>(dpi), 96);
      if (max_h > 0 && (min_h <= 0 || max_h >= min_h))
        mmi->ptMaxTrackSize.y = MulDiv(max_h, static_cast<int>(dpi), 96);
      return 0;
    }

    case WM_SIZE: {
      // lParam gives the new client area in physical pixels.
      // Skip minimised state - Windows reports 0x0 client size and we don't
      // want clients to see spurious zero-sized resize events.
      if (wParam == SIZE_MINIMIZED) return 0;

      auto* wud = get_wud(hwnd);
      if (wud) {
        UINT w_phys = LOWORD(lParam);
        UINT h_phys = HIWORD(lParam);
        wud->session->resize_render_ctx(wud->widget_index, w_phys, h_phys);

        auto* fwd = wud->session->get_widget(wud->widget_index);
        if (fwd) {
          int w_log = static_cast<int>(phys_to_log(static_cast<int>(w_phys), fwd->dpi));
          int h_log = static_cast<int>(phys_to_log(static_cast<int>(h_phys), fwd->dpi));
          fwd->width  = w_log;
          fwd->height = h_log;

          neui_event_t ev = {};
          ev.type                = NEUI_EVENT_RESIZE;
          ev.data.resize.widget  = { fwd->widget_id };
          ev.data.resize.width   = w_log;
          ev.data.resize.height  = h_log;
          wud->session->dispatch_event(&ev);
        }
      }
      return 0;
    }

    case WM_DPICHANGED: {
      // wParam: new DPI (HIWORD = Y, LOWORD = X - always equal on Windows).
      // lParam: RECT* suggesting the new window position/size at the new DPI.
      auto* wud = get_wud(hwnd);
      if (wud) {
        uint32_t new_dpi = static_cast<uint32_t>(LOWORD(wParam));
        wud->session->on_dpi_changed(wud->widget_index, new_dpi);

        // Resize to the system-suggested rect so the window keeps the same
        // logical size on the new monitor.
        const RECT* suggested = reinterpret_cast<const RECT*>(lParam);
        SetWindowPos(hwnd, nullptr,
          suggested->left, suggested->top,
          suggested->right  - suggested->left,
          suggested->bottom - suggested->top,
          SWP_NOZORDER | SWP_NOACTIVATE);
      }
      return 0;
    }

    // -----------------------------------------------------------------
    // Frame OS-focus relay. Only the frame HWND has real OS focus in this
    // host; no widget has its own HWND. When the user switches to / from
    // another app, replay WIDGET_FOCUS for the logically-focused widget so
    // clients see the same event timing as on the win32 host.

    case WM_KILLFOCUS: {
      auto* wud = get_wud(hwnd);
      if (wud && wud->session) {
        auto* s = wud->session;
        s->_os_focused = false;
        if (s->_focused_widget != 0 && s->_widgets.exists(s->_focused_widget)) {
          auto& wd = s->_widgets[s->_focused_widget];
          if (wd.emit_events) {
            neui_event_t ev = {};
            ev.type = NEUI_EVENT_WIDGET_FOCUS;
            ev.data.focus.widget  = { wd.widget_id };
            ev.data.focus.focused = false;
            s->dispatch_event(&ev);
          }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    case WM_SETFOCUS: {
      auto* wud = get_wud(hwnd);
      if (wud && wud->session) {
        auto* s = wud->session;
        s->_os_focused = true;
        if (s->_focused_widget != 0 && s->_widgets.exists(s->_focused_widget)) {
          auto& wd = s->_widgets[s->_focused_widget];
          if (wd.emit_events) {
            neui_event_t ev = {};
            ev.type = NEUI_EVENT_WIDGET_FOCUS;
            ev.data.focus.widget  = { wd.widget_id };
            ev.data.focus.focused = true;
            s->dispatch_event(&ev);
          }
        }
        InvalidateRect(hwnd, nullptr, FALSE);
      }
      return 0;
    }

    // -----------------------------------------------------------------
    // Mouse input

    case WM_MOUSEMOVE: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      auto* fwd = wud->session->get_widget(wud->widget_index);
      if (!fwd) break;

      // Arm WM_MOUSELEAVE notification on first move.
      if (!wud->tracking_mouse) {
        TRACKMOUSEEVENT tme = { sizeof(tme), TME_LEAVE, hwnd, 0 };
        TrackMouseEvent(&tme);
        wud->tracking_mouse = true;
      }

      float lx = phys_to_log(mouse_x(lParam), fwd->dpi);
      float ly = phys_to_log(mouse_y(lParam), fwd->dpi);

      // Popup-menu overlay (right-click context menu) takes priority over
      // everything else. While active it absorbs all hover updates inside
      // its rect and falls through to normal hover only when outside.
      if (wud->session->_popup_active) {
        wud->session->handle_popup_hover(lx, ly);
        return 0;
      }

      // Combo overlay scrollbar drag takes priority over all other mouse move handling.
      if (wud->session->handle_combo_scroll_drag(ly)) return 0;

      // Mouse hover over an open combo overlay highlights the row under the cursor
      // without committing selection (matches native Win32 combo behavior).
      if (wud->session->handle_combo_hover(lx, ly)) return 0;

      uint32_t hit = wud->session->widget_at(lx, ly, wud->widget_index);
      wud->session->set_hovered(hit);  // fires MOUSE_ENTER / MOUSE_LEAVE as needed

      // During a captured drag (button held), deliver MOUSE_MOVE to the widget
      // that received the original BUTTON_DOWN regardless of the hit test.
      uint32_t pressed = wud->session->_pressed_widget;
      uint32_t target  = (pressed != 0 && (wParam & MK_LBUTTON)) ? pressed : hit;

      if (target != 0) {
        auto* hw = wud->session->get_widget(target);
        if (hw) {
          neui_event_t ev = {};
          ev.type              = NEUI_EVENT_MOUSE_MOVE;
          ev.data.mouse.widget = { hw->widget_id };
          ev.data.mouse.x      = static_cast<int>(lx);
          ev.data.mouse.y      = static_cast<int>(ly);
          ev.data.mouse.buttonmap = static_cast<uint32_t>(wParam)
                                    & (MK_LBUTTON | MK_RBUTTON | MK_MBUTTON);
          wud->session->dispatch_mouse_event(target, &ev);
        }
      }
      return 0;
    }

    case WM_MOUSELEAVE: {
      auto* wud = get_wud(hwnd);
      if (wud) {
        wud->tracking_mouse = false;
        wud->session->set_hovered(0);  // fires MOUSE_LEAVE on current hover
      }
      return 0;
    }

    case WM_LBUTTONDOWN: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      auto* fwd = wud->session->get_widget(wud->widget_index);
      if (!fwd) break;

      float lx = phys_to_log(mouse_x(lParam), fwd->dpi);
      float ly = phys_to_log(mouse_y(lParam), fwd->dpi);

      // Popup menu overlay absorbs the click - picks an item or dismisses.
      // The nested message pump in open_popup_menu will see _popup_running
      // flip to false and return.
      if (wud->session->_popup_active) {
        wud->session->handle_popup_click(lx, ly);
        return 0;
      }

      // When a combo overlay is open, all clicks go to combo handling only.
      // The click is consumed regardless of where it lands (inside or outside overlay).
      if (wud->session->handle_combo_click(lx, ly)) {
        // If a scrollbar drag was started in the overlay, capture the mouse so
        // WM_MOUSEMOVE keeps arriving even when the cursor leaves the window.
        if (wud->session->_combo_sb_dragging)
          SetCapture(hwnd);
        return 0;
      }

      uint32_t hit = wud->session->widget_at(lx, ly, wud->widget_index);

      // Focus always follows the primary click, even without emit_events.
      wud->session->set_focus(hit);
      wud->session->set_pressed(hit);

      SetCapture(hwnd);  // keep getting mouse messages if cursor leaves the window

      if (hit != 0) {
        auto* hw = wud->session->get_widget(hit);
        if (hw) {
          neui_event_t ev = {};
          ev.type              = NEUI_EVENT_MOUSE_BUTTON_DOWN;
          ev.data.mouse.widget = { hw->widget_id };
          ev.data.mouse.x      = static_cast<int>(lx);
          ev.data.mouse.y      = static_cast<int>(ly);
          ev.data.mouse.buttonmap = static_cast<uint32_t>(wParam);
          wud->session->dispatch_mouse_event(hit, &ev);
        }
      }
      return 0;
    }

    case WM_LBUTTONDBLCLK: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      auto* fwd = wud->session->get_widget(wud->widget_index);
      if (!fwd) break;

      float lx = phys_to_log(mouse_x(lParam), fwd->dpi);
      float ly = phys_to_log(mouse_y(lParam), fwd->dpi);

      uint32_t hit = wud->session->widget_at(lx, ly, wud->widget_index);
      if (hit != 0) {
        auto* hw = wud->session->get_widget(hit);
        if (hw) {
          neui_event_t ev = {};
          ev.type                 = NEUI_EVENT_MOUSE_BUTTON_DBLCLICK;
          ev.data.mouse.widget    = { hw->widget_id };
          ev.data.mouse.x         = static_cast<int>(lx);
          ev.data.mouse.y         = static_cast<int>(ly);
          ev.data.mouse.buttonmap = static_cast<uint32_t>(wParam);
          wud->session->dispatch_mouse_event(hit, &ev);
        }
      }
      return 0;
    }

    case WM_LBUTTONUP: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      auto* fwd = wud->session->get_widget(wud->widget_index);
      if (!fwd) break;

      // End a combo overlay scrollbar drag if one was active.
      if (wud->session->_combo_sb_dragging) {
        wud->session->_combo_sb_dragging = false;
        ReleaseCapture();
        return 0;
      }

      ReleaseCapture();

      float lx = phys_to_log(mouse_x(lParam), fwd->dpi);
      float ly = phys_to_log(mouse_y(lParam), fwd->dpi);

      uint32_t hit     = wud->session->widget_at(lx, ly, wud->widget_index);
      uint32_t pressed = wud->session->_pressed_widget;
      wud->session->set_pressed(0);

      if (hit != 0) {
        auto* hw = wud->session->get_widget(hit);
        if (hw) {
          neui_event_t ev = {};
          ev.data.mouse.widget    = { hw->widget_id };
          ev.data.mouse.x         = static_cast<int>(lx);
          ev.data.mouse.y         = static_cast<int>(ly);
          ev.data.mouse.buttonmap = 0;

          ev.type = NEUI_EVENT_MOUSE_BUTTON_UP;
          wud->session->dispatch_mouse_event(hit, &ev);

          // A click is UP on the same widget as DOWN.
          if (hit == pressed) {
            ev.type = NEUI_EVENT_MOUSE_BUTTON_CLICK;
            wud->session->dispatch_mouse_event(hit, &ev);
          }
        }
      }
      return 0;
    }

    case WM_RBUTTONDOWN: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      auto* fwd = wud->session->get_widget(wud->widget_index);
      if (!fwd) break;
      float lx = phys_to_log(mouse_x(lParam), fwd->dpi);
      float ly = phys_to_log(mouse_y(lParam), fwd->dpi);
      // If a popup is up, a right-click outside dismisses it.
      if (wud->session->_popup_active) {
        wud->session->handle_popup_click(lx, ly);
        return 0;
      }
      uint32_t hit = wud->session->widget_at(lx, ly, wud->widget_index);
      if (hit == 0) return 0;
      auto* hw = wud->session->get_widget(hit);
      if (!hw) return 0;
      neui_event_t ev = {};
      ev.type                 = NEUI_EVENT_MOUSE_RBUTTON_DOWN;
      ev.data.mouse.widget    = { hw->widget_id };
      ev.data.mouse.x         = static_cast<int>(lx);
      ev.data.mouse.y         = static_cast<int>(ly);
      ev.data.mouse.buttonmap = 0;
      wud->session->dispatch_mouse_event(hit, &ev);
      return 0;
    }

    case WM_RBUTTONUP: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      auto* fwd = wud->session->get_widget(wud->widget_index);
      if (!fwd) break;
      float lx = phys_to_log(mouse_x(lParam), fwd->dpi);
      float ly = phys_to_log(mouse_y(lParam), fwd->dpi);
      uint32_t hit = wud->session->widget_at(lx, ly, wud->widget_index);
      if (hit == 0) return 0;
      auto* hw = wud->session->get_widget(hit);
      if (!hw) return 0;
      neui_event_t ev = {};
      ev.type                 = NEUI_EVENT_MOUSE_RBUTTON_UP;
      ev.data.mouse.widget    = { hw->widget_id };
      ev.data.mouse.x         = static_cast<int>(lx);
      ev.data.mouse.y         = static_cast<int>(ly);
      ev.data.mouse.buttonmap = 0;
      wud->session->dispatch_mouse_event(hit, &ev);
      return 0;
    }

    case WM_MOUSEWHEEL: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      auto* fwd = wud->session->get_widget(wud->widget_index);
      if (!fwd) break;

      // WM_MOUSEWHEEL gives screen coordinates - convert to client.
      POINT pt = { mouse_x(lParam), mouse_y(lParam) };
      ScreenToClient(hwnd, &pt);
      float lx = phys_to_log(pt.x, fwd->dpi);
      float ly = phys_to_log(pt.y, fwd->dpi);

      // Scale raw wheel delta to lines using the system scroll preference.
      int raw_delta = GET_WHEEL_DELTA_WPARAM(wParam);
      UINT scroll_lines = 3;
      SystemParametersInfoW(SPI_GETWHEELSCROLLLINES, 0, &scroll_lines, 0);
      int delta = (raw_delta * static_cast<int>(scroll_lines)) / WHEEL_DELTA;

      // Combo overlay intercepts wheel when cursor is over the drop area.
      if (wud->session->handle_combo_wheel(lx, ly, delta)) return 0;

      uint32_t hit = wud->session->widget_at(lx, ly, wud->widget_index);
      if (hit == 0) break;
      auto* hw = wud->session->get_widget(hit);
      if (!hw) break;

      // GRID + SMOOTH mode: feed the raw pixel-precise delta into the shared
      // kinetics so Win32 gets the same rubber-band + spring-back behaviour
      // as macOS. The line-quantized `delta` above is dropped on this path
      // since the kinetics integrator owns its own pixel accumulator.
      if (neui_detail::GridModel* model = hw->grid_model_ptr()) {
        using namespace neui_detail;
        auto cfg = grid_read_config(hw->attrs.get());
        if (grid_smooth_enabled(cfg, /*platform_default_smooth=*/false)) {
          GridViewport vp = grid_compute_viewport(*model, hw->width, hw->height,
                                                    cfg.row_h, cfg.header_h);
          double notches = (double)raw_delta / (double)WHEEL_DELTA;
          GridWheelInput in;
          in.precise  = true;                 // delta_px is already in px
          in.delta_px = notches * (double)scroll_lines * (double)cfg.row_h;
          GridWheelAction act = grid_scroll_wheel(*model, vp, cfg.row_h, in);
          if (act.changed) InvalidateRect(hwnd, nullptr, FALSE);
          if (act.start_bounce) {
            wud->bouncing_grid_index = hit;
            SetTimer(hwnd, XPL_GRID_BOUNCE_TIMER_ID, 16, nullptr);
          }
          return 0;
        }
      }

      neui_event_t ev = {};
      ev.type              = NEUI_EVENT_MOUSE_WHEEL;
      ev.data.wheel.widget = { hw->widget_id };
      ev.data.wheel.x      = static_cast<int>(lx);
      ev.data.wheel.y      = static_cast<int>(ly);
      ev.data.wheel.delta  = delta;
      wud->session->dispatch_mouse_event(hit, &ev);
      return 0;
    }

    case WM_TIMER: {
      if (wParam != XPL_GRID_BOUNCE_TIMER_ID) break;
      auto* wud = get_wud(hwnd);
      if (!wud) { KillTimer(hwnd, XPL_GRID_BOUNCE_TIMER_ID); return 0; }
      auto* hw = (wud->bouncing_grid_index != 0)
                   ? wud->session->get_widget(wud->bouncing_grid_index)
                   : nullptr;
      neui_detail::GridModel* model = hw ? hw->grid_model_ptr() : nullptr;
      if (!model) {
        wud->bouncing_grid_index = 0;
        KillTimer(hwnd, XPL_GRID_BOUNCE_TIMER_ID);
        return 0;
      }
      using namespace neui_detail;
      auto cfg = grid_read_config(hw->attrs.get());
      GridViewport vp = grid_compute_viewport(*model, hw->width, hw->height,
                                                cfg.row_h, cfg.header_h);
      bool more = grid_scroll_bounce_step(*model, vp, cfg.row_h);
      InvalidateRect(hwnd, nullptr, FALSE);
      if (!more) {
        wud->bouncing_grid_index = 0;
        KillTimer(hwnd, XPL_GRID_BOUNCE_TIMER_ID);
      }
      return 0;
    }

    case WM_KEYDOWN: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      // Popup menu absorbs Esc / Enter / Space / arrow keys.
      if (wud->session->_popup_active &&
          wud->session->handle_popup_key(static_cast<uint32_t>(wParam))) {
        return 0;
      }
      if (wParam == VK_TAB) {
        bool shift = (GetKeyState(VK_SHIFT) & 0x8000) != 0;
        wud->session->focus_next(!shift);
        return 0;  // consumed - do not feed to DefWindowProc
      }
      dispatch_key_to_focused(wud->session, NEUI_EVENT_KEYDOWN,
                               static_cast<uint32_t>(wParam));
      break;
    }

    case WM_KEYUP: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      dispatch_key_to_focused(wud->session, NEUI_EVENT_KEYUP,
                               static_cast<uint32_t>(wParam));
      break;
    }

    case WM_CHAR: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;

      // WM_CHAR delivers UTF-16 code units. Assemble surrogate pairs into a
      // full Unicode codepoint before forwarding.
      WCHAR ch = static_cast<WCHAR>(wParam);
      uint32_t codepoint;

      if (ch >= 0xD800 && ch <= 0xDBFF) {
        // High surrogate - store and wait for the low surrogate.
        wud->pending_surrogate = ch;
        return 0;
      }
      if (ch >= 0xDC00 && ch <= 0xDFFF) {
        // Low surrogate - combine with the stored high surrogate.
        if (wud->pending_surrogate != 0) {
          codepoint = 0x10000u
            + (static_cast<uint32_t>(wud->pending_surrogate - 0xD800) << 10)
            + static_cast<uint32_t>(ch - 0xDC00);
          wud->pending_surrogate = 0;
        } else {
          // Stray low surrogate with no prior high - discard.
          return 0;
        }
      } else {
        wud->pending_surrogate = 0;  // clear any stale high surrogate
        codepoint = static_cast<uint32_t>(ch);
      }

      dispatch_key_to_focused(wud->session, NEUI_EVENT_KEYCHAR, codepoint);
      break;
    }

    // -----------------------------------------------------------------
    // IME composition. The xpl host does all rendering itself, including
    // the in-progress composition string with its underline - so we mask
    // the OS default composition UI bit. The candidate window (kanji
    // picker etc.) is OS-rendered and positioned via
    // ImmSetCompositionWindow / ImmSetCandidateWindow at the caret of the
    // focused text widget.

    case WM_IME_SETCONTEXT:
      // Suppress OS default composition UI; keep candidate UI.
      return DefWindowProcW(hwnd, msg, wParam,
        lParam & ~ISC_SHOWUICOMPOSITIONWINDOW);

    case WM_IME_STARTCOMPOSITION: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      auto* frame_wd = wud->session->get_widget(wud->widget_index);
      WidgetData* tw = focused_text_widget(wud->session);
      if (tw && tw->on_composition(WidgetData::COMP_START, nullptr, 0, 0, nullptr)) {
        set_composition_window_at_caret(hwnd, wud->session, tw, frame_wd);
        return 0;  // consume - do not let DefWindowProc spawn a default UI
      }
      break;
    }

    case WM_IME_COMPOSITION: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      auto* frame_wd = wud->session->get_widget(wud->widget_index);
      WidgetData* tw = focused_text_widget(wud->session);
      if (!tw) break;
      HIMC himc = ImmGetContext(hwnd);
      if (!himc) break;

      // Result string commits first (final text from the previous
      // composition), then the new composition string updates.
      bool handled = false;
      if (lParam & GCS_RESULTSTR) {
        LONG bytes = ImmGetCompositionStringW(himc, GCS_RESULTSTR, nullptr, 0);
        std::string utf8;
        if (bytes > 0) {
          std::wstring w(static_cast<size_t>(bytes / sizeof(wchar_t)), L'\0');
          ImmGetCompositionStringW(himc, GCS_RESULTSTR, w.data(), bytes);
          utf8 = utf16_to_utf8(w.data(), static_cast<int>(w.size()));
        }
        handled |= tw->on_composition(WidgetData::COMP_RESULT,
                                       utf8.c_str(),
                                       static_cast<int>(utf8.size()),
                                       0, nullptr);
      }
      if (lParam & GCS_COMPSTR) {
        LONG bytes = ImmGetCompositionStringW(himc, GCS_COMPSTR, nullptr, 0);
        std::wstring w;
        if (bytes > 0) {
          w.resize(static_cast<size_t>(bytes / sizeof(wchar_t)));
          ImmGetCompositionStringW(himc, GCS_COMPSTR, w.data(), bytes);
        }
        // Caret position is reported in UTF-16 code units; convert.
        int caret_w = static_cast<int>(
          ImmGetCompositionStringW(himc, GCS_CURSORPOS, nullptr, 0));
        std::string utf8 = utf16_to_utf8(w.data(), static_cast<int>(w.size()));
        int caret_b = utf16_caret_to_utf8(w.data(),
                                           static_cast<int>(w.size()),
                                           caret_w);

        // Per-clause attribute info (one byte per UTF-16 code unit).
        // Fetch and translate to per-UTF-8-byte for the widget.
        std::vector<uint8_t> utf8_attrs;
        if (lParam & GCS_COMPATTR) {
          LONG abytes = ImmGetCompositionStringW(himc, GCS_COMPATTR,
                                                  nullptr, 0);
          if (abytes > 0 && static_cast<size_t>(abytes) >= w.size()) {
            std::vector<uint8_t> w_attrs(static_cast<size_t>(abytes), 0);
            ImmGetCompositionStringW(himc, GCS_COMPATTR,
                                      w_attrs.data(), abytes);
            utf8_attrs = compattr_to_utf8(w.data(),
                                           static_cast<int>(w.size()),
                                           w_attrs.data());
          }
        }
        handled |= tw->on_composition(WidgetData::COMP_UPDATE,
                                       utf8.c_str(),
                                       static_cast<int>(utf8.size()),
                                       caret_b,
                                       utf8_attrs.empty() ? nullptr : utf8_attrs.data());
      }
      ImmReleaseContext(hwnd, himc);
      if (handled) {
        set_composition_window_at_caret(hwnd, wud->session, tw, frame_wd);
        return 0;  // consume - widget handled it and we drew it ourselves
      }
      break;  // fall through to DefWindowProc when widget refused composition
    }

    case WM_IME_ENDCOMPOSITION: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      WidgetData* tw = focused_text_widget(wud->session);
      if (tw) tw->on_composition(WidgetData::COMP_END, nullptr, 0, 0, nullptr);
      return 0;
    }

    case WM_INITMENUPOPUP: {
      // Combine three signals to decide each item's enabled state:
      //   1. static `mi.enabled` (set via tree->set_enabled)
      //   2. for built-in commands, can_focused_perform_command(cmd)
      //   3. opt-in client validate callback (NEUI_API_MENU_CLIENT) - if
      //      registered, runs for every non-separator item including those
      //      without menu_cmd and those with user-range cmds.
      // wParam = HMENU of the popup being shown.
      auto* wud = get_wud(hwnd);
      if (!wud || !wud->session) break;
      HMENU popup = reinterpret_cast<HMENU>(wParam);
      Session* sess = wud->session;
      bool has_validate = sess->_menu_client && sess->_menu_client->validate;
      for (uint32_t mb_idx : sess->_menubars) {
        if (!sess->_widgets.exists(mb_idx)) continue;
        auto* mw = dynamic_cast<MenubarWidget*>(&sess->_widgets[mb_idx]);
        if (!mw) continue;
        for (auto& kv : mw->menu_items) {
          uint32_t neui_id = kv.first;
          auto& mi = kv.second;
          if (mi.is_separator) continue;
          if (mi.parent_hmenu != popup) continue;
          if (!has_validate &&
              (mi.menu_cmd == 0 || mi.menu_cmd >= NEUI_CMD_USER_BASE))
            continue;  // nothing to compute for this item

          bool enabled = mi.enabled;
          if (enabled && mi.menu_cmd != 0 &&
              mi.menu_cmd < NEUI_CMD_USER_BASE) {
            enabled = sess->can_focused_perform_command(mi.menu_cmd);
          }
          if (enabled && has_validate) {
            enabled = sess->_menu_client->validate(
              sess->get_token(),
              { mw->widget_id }, { neui_id }, mi.menu_cmd);
          }
          EnableMenuItem(popup, mi.cmd_id,
                          MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
        }
      }
      return 0;
    }

    case WM_COMMAND: {
      // Menu pick (HIWORD=0) or accelerator (HIWORD=1) - both have lParam == 0.
      // Child control notifications have lParam != 0 (no controls have HWNDs
      // in this host, so notifications shouldn't appear here, but the guard
      // is cheap and keeps semantics consistent with the win32 host).
      if (lParam == 0) {
        auto* wud = get_wud(hwnd);
        if (wud)
          wud->session->dispatch_menu_event(LOWORD(wParam));
      }
      return 0;
    }

    default:
      break;
    }
    return DefWindowProcW(hwnd, msg, wParam, lParam);
  }

  // -------------------------------------------------------------------------

  void platform_init()
  {
    if (g_class_registered) return;
    g_hinstance = GetModuleHandleW(nullptr);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = XplWndProc;
    wc.hInstance     = g_hinstance;
    wc.hCursor       = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = nullptr;  // D2D handles painting
    wc.lpszClassName = k_wndclass;
    RegisterClassExW(&wc);
    g_class_registered = true;
  }

  neui_render_backend_t* platform_get_backend()
  {
    return neui_d2d_backend::get_backend();
  }

  float platform_get_scale_factor(void* native_handle)
  {
    if (!native_handle) return 1.0f;
    UINT dpi = GetDpiForWindow(static_cast<HWND>(native_handle));
    return static_cast<float>(dpi) / 96.0f;
  }

  // Helper: create a HWND and attach a D2D render context to wd.
  // Logical coordinates in wd are converted to physical pixels using the
  // system DPI before the window exists, then refined to per-monitor DPI after.
  static void create_native_window(Session* session, uint32_t widget_index,
                                    WidgetData& wd, DWORD style, DWORD ex_style,
                                    HWND owner_hwnd = nullptr)
  {
    // Use system DPI as the best estimate of the target monitor's DPI before
    // the window is created. The actual per-monitor DPI is read back afterwards.
    UINT sys_dpi = GetDpiForSystem();

    auto* wud = new WindowUserData{ session, widget_index };

    HWND hwnd = CreateWindowExW(
      ex_style,
      k_wndclass,
      L"",
      style,
      MulDiv(wd.x,      sys_dpi, 96),
      MulDiv(wd.y,      sys_dpi, 96),
      MulDiv(wd.width,  sys_dpi, 96),
      MulDiv(wd.height, sys_dpi, 96),
      owner_hwnd,
      nullptr,
      g_hinstance,
      wud   // lpCreateParams -> stored in GWLP_USERDATA via WM_NCCREATE
    );

    if (!hwnd) { delete wud; return; }

    // Read back the actual per-monitor DPI now that the window has a monitor.
    wd.dpi           = GetDpiForWindow(hwnd);
    wd.native_handle = hwnd;

    // If the window's actual monitor DPI differs from GetDpiForSystem() (e.g.
    // the primary is at 100% but the window opened on a 150% secondary, or
    // vice versa), our initial CreateWindowExW used the wrong scaling. The
    // D2D render target uses the per-window DPI for widget coordinates, so a
    // mismatch leaves the outer window sized at one scale and the widget
    // coordinate space at another - visible as margins that don't match
    // left/top. Resize using the actual window DPI to keep both in sync.
    if (wd.dpi != sys_dpi && wd.dpi != 0) {
      SetWindowPos(hwnd, nullptr, 0, 0,
                   MulDiv(wd.width,  static_cast<int>(wd.dpi), 96),
                   MulDiv(wd.height, static_cast<int>(wd.dpi), 96),
                   SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }

    // System-theme side effects (DWM dark title bar, uxtheme HMENU dark
    // mode) are gated by NEUI_ATTR_FOLLOW_SYSTEM_THEME = 1 on this frame.
    // Default (unset / 0) keeps the OS-default chrome - same contract as
    // the win32 native host. The painted client area still pulls colours
    // from current_palette(); a frame that wants a frozen non-system look
    // can pair this with NEUI_ATTR_THEME_MODE = LIGHT / DARK on the session.
    if (wd.attrs && wd.attrs->get_int(NEUI_ATTR_FOLLOW_SYSTEM_THEME, 0) != 0) {
      // Scope the override to THIS session so the is_dark read reflects
      // its effective palette (not a sibling session's last write).
      neui_detail::ScopedPaletteOverride scope(
        wd.session ? wd.session->effective_palette_ptr() : nullptr);
      bool is_dark = neui_detail::current_palette().is_dark;
      neui_detail::set_app_dark_preference(is_dark);
      neui_detail::apply_dark_window_mode(hwnd, is_dark);
      BOOL dark_attr = is_dark ? TRUE : FALSE;
      if (FAILED(DwmSetWindowAttribute(hwnd, 20, &dark_attr, sizeof(dark_attr))))
        DwmSetWindowAttribute(hwnd, 19, &dark_attr, sizeof(dark_attr));
    }

    // Apply any title text set before show.
    if (!wd.text.empty())
      SetWindowTextW(hwnd, to_wide(wd.text.c_str()).c_str());

    // Apply any icon attribute set before show.
    if (wd.attrs) {
      const char* icon_path = wd.attrs->get_string(NEUI_ATTR_ICON_PATH);
      if (icon_path && *icon_path) {
        HICON owned = nullptr;
        neui_detail::apply_window_icon(hwnd, icon_path, &owned);
        wd.native_icon = owned;  // store as void* for later free
      }
    }

    // Create the D2D render context with the physical client area dimensions.
    RECT rc;
    GetClientRect(hwnd, &rc);
    auto* backend = platform_get_backend();
    if (backend) {
      wd.render_ctx = backend->create_context(
        hwnd,
        static_cast<uint32_t>(rc.right  - rc.left),
        static_cast<uint32_t>(rc.bottom - rc.top));
    }
  }

  void platform_create_appwindow(Session* session, uint32_t widget_index,
                                  WidgetData& wd)
  {
    create_native_window(session, widget_index, wd,
      WS_OVERLAPPEDWINDOW,
      0);
  }

  void platform_create_plugwindow(Session* session, uint32_t widget_index,
                                   WidgetData& wd)
  {
    create_native_window(session, widget_index, wd,
      WS_POPUP | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
      0);
  }

  void platform_create_dialog(Session* session, uint32_t widget_index,
                               WidgetData& wd, void* owner_native)
  {
    // Auto-centre over owner when the client did not supply a position.
    // (0, 0) is the conventional "no position chosen" sentinel; clients
    // that want a hard top-left can call set_pos with explicit coords.
    if (owner_native && wd.x == 0 && wd.y == 0) {
      RECT or_rc;
      if (GetWindowRect(static_cast<HWND>(owner_native), &or_rc)) {
        UINT odpi = GetDpiForWindow(static_cast<HWND>(owner_native));
        if (odpi == 0) odpi = 96;
        // Convert owner physical rect -> logical pixels at 96 DPI; reading
        // wd.x/y back out from CreateWindow happens at the same conversion.
        int or_w_log = MulDiv(or_rc.right  - or_rc.left, 96, static_cast<int>(odpi));
        int or_h_log = MulDiv(or_rc.bottom - or_rc.top,  96, static_cast<int>(odpi));
        int or_x_log = MulDiv(or_rc.left, 96, static_cast<int>(odpi));
        int or_y_log = MulDiv(or_rc.top,  96, static_cast<int>(odpi));
        wd.x = or_x_log + (or_w_log - wd.width)  / 2;
        wd.y = or_y_log + (or_h_log - wd.height) / 2;
        if (wd.x < 0) wd.x = 0;
        if (wd.y < 0) wd.y = 0;
      }
    }

    // Standard non-resizable dialog chrome: titlebar + system menu (close
    // button), no min/max boxes, no thick frame. Owner relationship is
    // established via the hwndParent slot of CreateWindowEx so Win32 also
    // gets the correct Z-ordering and re-activation behaviour for free.
    create_native_window(session, widget_index, wd,
      WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
      WS_EX_DLGMODALFRAME,
      static_cast<HWND>(owner_native));
  }

  void platform_set_window_enabled(void* native_handle, bool enabled)
  {
    if (!native_handle) return;
    EnableWindow(static_cast<HWND>(native_handle), enabled ? TRUE : FALSE);
  }

  void platform_activate_window(void* native_handle)
  {
    if (!native_handle) return;
    HWND h = static_cast<HWND>(native_handle);
    SetForegroundWindow(h);
    SetActiveWindow(h);
  }

  void platform_destroy_window(WidgetData& wd)
  {
    if (wd.native_handle) {
      DestroyWindow(static_cast<HWND>(wd.native_handle));
      // native_handle and render_ctx are cleared in WM_DESTROY / WM_NCDESTROY.
    }
    if (wd.native_icon) {
      HICON h = static_cast<HICON>(wd.native_icon);
      DestroyIcon(h);
      wd.native_icon = nullptr;
    }
  }

  void platform_show_window(void* native_handle)
  {
    ShowWindow(static_cast<HWND>(native_handle), SW_SHOW);
    UpdateWindow(static_cast<HWND>(native_handle));
  }

  void platform_hide_window(void* native_handle)
  {
    ShowWindow(static_cast<HWND>(native_handle), SW_HIDE);
  }

  void platform_set_window_title(void* native_handle, const char* text)
  {
    if (!native_handle) return;
    SetWindowTextW(static_cast<HWND>(native_handle),
                   to_wide(text ? text : "").c_str());
  }

  void platform_set_window_pos(void* native_handle,
                                int x, int y, int w, int h, uint32_t dpi)
  {
    // Client coordinates are logical (96 DPI base); convert to physical pixels.
    SetWindowPos(static_cast<HWND>(native_handle), nullptr,
      MulDiv(x, dpi, 96),
      MulDiv(y, dpi, 96),
      MulDiv(w, dpi, 96),
      MulDiv(h, dpi, 96),
      SWP_NOZORDER | SWP_NOACTIVATE);
  }

  void platform_post_close(void* native_handle)
  {
    PostMessageW(static_cast<HWND>(native_handle), WM_CLOSE, 0, 0);
  }

  void platform_invalidate(void* native_handle)
  {
    if (native_handle)
      InvalidateRect(static_cast<HWND>(native_handle), nullptr, FALSE);
  }

  static void dispatch_one(MSG& msg)
  {
    // Menu accelerator translation runs before TranslateMessage so a
    // bound shortcut takes priority over widget-local key handling.
    // No IsDialogMessage call: the xpl host has its own Tab traversal
    // (Session::focus_next) and lets IME composition see Enter/Esc.
    bool consumed = false;
    for (auto& s : sessions) {
      if (s && s->try_translate_accel(&msg)) { consumed = true; break; }
    }
    if (consumed) return;

    TranslateMessage(&msg);
    DispatchMessageW(&msg);
  }

  bool platform_run()
  {
    MSG msg = {};
    while (GetMessageW(&msg, nullptr, 0, 0)) {
      dispatch_one(msg);
    }
    return true;
  }

  // Drain pending messages without blocking. Returns false when WM_QUIT
  // was seen, true otherwise.
  bool platform_pump_once()
  {
    MSG msg = {};
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (msg.message == WM_QUIT) return false;
      dispatch_one(msg);
    }
    return true;
  }

  // Nested pump for popup menus. Same dispatch rules as platform_run, just
  // gated on the caller's keep-going flag.
  bool platform_run_modal_until(bool* keep_running)
  {
    if (!keep_running) return true;
    MSG msg = {};
    while (*keep_running) {
      BOOL r = GetMessageW(&msg, nullptr, 0, 0);
      if (r == 0) {
        // WM_QUIT - re-post so the outer pump sees it after we return.
        PostQuitMessage(static_cast<int>(msg.wParam));
        return false;
      }
      if (r == -1) return false;  // GetMessage error
      dispatch_one(msg);
    }
    return true;
  }

  // -------------------------------------------------------------------------
  // Native menu bar

  void* platform_menubar_create(uint32_t /*menubar_widget_id*/)
  {
    // The widget_id arg is for the macOS impl's routing map; Win32 doesn't
    // need it (WM_COMMAND already arrives at the right frame).
    return static_cast<void*>(CreateMenu());
  }

  void platform_menubar_destroy(void* hmenu)
  {
    if (hmenu) DestroyMenu(static_cast<HMENU>(hmenu));
  }

  void platform_menubar_attach(void* frame_hwnd, void* hmenu)
  {
    if (!frame_hwnd || !hmenu) return;
    SetMenu(static_cast<HWND>(frame_hwnd), static_cast<HMENU>(hmenu));
    DrawMenuBar(static_cast<HWND>(frame_hwnd));
  }

  void platform_menubar_refresh(void* frame_hwnd)
  {
    if (frame_hwnd) DrawMenuBar(static_cast<HWND>(frame_hwnd));
  }

  void* platform_menubar_add_popup(void* hmenu, const char* display_text)
  {
    if (!hmenu) return nullptr;
    HMENU popup = CreatePopupMenu();
    if (!popup) return nullptr;
    auto wtext = to_wide(display_text);
    AppendMenuW(static_cast<HMENU>(hmenu), MF_POPUP,
                reinterpret_cast<UINT_PTR>(popup), wtext.c_str());
    return static_cast<void*>(popup);
  }

  void platform_menubar_add_item(void* parent_hmenu, uint32_t cmd_id,
                                  const char* display_text)
  {
    if (!parent_hmenu) return;
    auto wtext = to_wide(display_text);
    AppendMenuW(static_cast<HMENU>(parent_hmenu), MF_STRING, cmd_id, wtext.c_str());
  }

  void platform_menubar_add_separator(void* parent_hmenu, uint32_t cmd_id)
  {
    if (!parent_hmenu) return;
    MENUITEMINFOW mii = {};
    mii.cbSize = sizeof(mii);
    mii.fMask  = MIIM_TYPE | MIIM_ID;
    mii.fType  = MFT_SEPARATOR;
    mii.wID    = cmd_id;
    InsertMenuItemW(static_cast<HMENU>(parent_hmenu),
                    static_cast<UINT>(GetMenuItemCount(static_cast<HMENU>(parent_hmenu))),
                    TRUE, &mii);
  }

  void platform_menubar_remove_popup(void* parent_hmenu, void* submenu)
  {
    if (!parent_hmenu || !submenu) return;
    RemoveMenu(static_cast<HMENU>(parent_hmenu),
               reinterpret_cast<UINT_PTR>(static_cast<HMENU>(submenu)),
               MF_BYCOMMAND);
    DestroyMenu(static_cast<HMENU>(submenu));
  }

  void platform_menubar_remove_item(void* parent_hmenu, uint32_t cmd_id)
  {
    if (!parent_hmenu) return;
    RemoveMenu(static_cast<HMENU>(parent_hmenu), cmd_id, MF_BYCOMMAND);
  }

  void platform_menubar_enable_item(void* parent_hmenu, uint32_t cmd_id, bool enabled)
  {
    if (!parent_hmenu) return;
    EnableMenuItem(static_cast<HMENU>(parent_hmenu), cmd_id,
                   MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
  }

  void platform_menubar_enable_popup(void* parent_hmenu, void* submenu, bool enabled)
  {
    if (!parent_hmenu || !submenu) return;
    // HMENU is a pointer; cannot pass it as UINT via MF_BYCOMMAND.
    // Find it by position in the parent menu instead.
    HMENU hpar = static_cast<HMENU>(parent_hmenu);
    HMENU hsub = static_cast<HMENU>(submenu);
    int count = GetMenuItemCount(hpar);
    for (int i = 0; i < count; ++i) {
      if (GetSubMenu(hpar, i) == hsub) {
        EnableMenuItem(hpar, static_cast<UINT>(i),
                       MF_BYPOSITION | (enabled ? MF_ENABLED : MF_GRAYED));
        break;
      }
    }
  }

  void platform_menubar_set_item_text(void* parent_hmenu, uint32_t cmd_id,
                                       const char* display_text)
  {
    if (!parent_hmenu) return;
    auto wtext = to_wide(display_text);
    ModifyMenuW(static_cast<HMENU>(parent_hmenu), cmd_id,
                MF_BYCOMMAND | MF_STRING, cmd_id, wtext.c_str());
  }

  void platform_menubar_set_item_shortcut(void* /*parent_hmenu*/,
                                           uint32_t /*cmd_id*/,
                                           uint32_t /*modifiers*/,
                                           uint32_t /*key*/)
  {
    // Win32 routes shortcuts through the per-menubar HACCEL rebuilt by
    // rebuild_menubar_accel - set_item_shortcut is a no-op here. The
    // tab+suffix in the menu item's display text comes from
    // platform_menubar_set_item_text, called by the same t_set_shortcut path.
  }

  void platform_set_window_icon(WidgetData& wd, const char* path_utf8)
  {
    if (!wd.native_handle || !path_utf8 || !*path_utf8) return;
    HICON owned = static_cast<HICON>(wd.native_icon);
    neui_detail::apply_window_icon(static_cast<HWND>(wd.native_handle),
                                    path_utf8, &owned);
    wd.native_icon = owned;
  }

  void platform_apply_size_constraints(void* /*native_handle*/,
                                        int /*min_w*/, int /*min_h*/,
                                        int /*max_w*/, int /*max_h*/)
  {
    // Win32 pulls min/max from the widget's attribute bag inside
    // WM_GETMINMAXINFO at every track event, so there's no separate state to
    // push. Triggering a fresh evaluation isn't necessary either - the next
    // user drag-resize / SetWindowPos call will hit the new values.
  }

  // -------------------------------------------------------------------------
  // Image loading - thin wrapper over the shared WIC decoder in
  // hosts/shared/win32/image_loader_win32.h (resource-first, then disk).

  uint8_t* platform_load_image(const char* path,
                                uint32_t* width_out, uint32_t* height_out)
  {
    return neui_detail::load_image_bgra8_w32(path, width_out, height_out);
  }

  void platform_free_image(uint8_t* pixels)
  {
    neui_detail::free_image_bgra8_w32(pixels);
  }

  // -------------------------------------------------------------------------
  // System clipboard - delegates to the header-only Win32 helpers.

  bool platform_clipboard_set_text(const char* utf8, uint32_t length)
  {
    return neui_detail::clipboard_set_text_win32(utf8, length);
  }

  int platform_clipboard_get_text(char* buf, int buflen)
  {
    return neui_detail::clipboard_get_text_win32(buf, buflen);
  }

  bool platform_clipboard_has_text()
  {
    return neui_detail::clipboard_has_text_win32();
  }

  bool platform_clipboard_write_item(const neui_detail::DataItem& item)
  {
    return neui_detail::clipboard_write_item_win32(item);
  }

  bool platform_clipboard_read_item(neui_detail::DataItem& item)
  {
    return neui_detail::clipboard_read_item_win32(item);
  }

  // -------------------------------------------------------------------------
  // Drag & drop. The seam callbacks below thunk into the xpl Session;
  // each Session::dispatch_dnd_* converts to a NEUI_EVENT_DND_* and walks
  // the widget tree for the deepest drop_target match.

  static uint32_t xpl_dnd_on_enter(void* session_ptr, uint32_t frame_widget_id,
                                    int x, int y,
                                    const char* const* formats,
                                    uint32_t formats_count,
                                    uint32_t suggested,
                                    uint32_t buttonmap)
  {
    auto* s = static_cast<Session*>(session_ptr);
    if (!s) return 0;
    return s->dispatch_dnd_enter(frame_widget_id & 0xFFFFu,
                                  x, y, formats, formats_count,
                                  suggested, buttonmap);
  }

  static uint32_t xpl_dnd_on_move(void* session_ptr, uint32_t frame_widget_id,
                                   int x, int y,
                                   const char* const* formats,
                                   uint32_t formats_count,
                                   uint32_t suggested,
                                   uint32_t buttonmap)
  {
    auto* s = static_cast<Session*>(session_ptr);
    if (!s) return 0;
    return s->dispatch_dnd_move(frame_widget_id & 0xFFFFu,
                                 x, y, formats, formats_count,
                                 suggested, buttonmap);
  }

  static void xpl_dnd_on_leave(void* session_ptr)
  {
    auto* s = static_cast<Session*>(session_ptr);
    if (!s) return;
    s->dispatch_dnd_leave();
  }

  static uint32_t xpl_dnd_on_drop(void* session_ptr, uint32_t frame_widget_id,
                                   int x, int y,
                                   const char* const* formats,
                                   uint32_t formats_count,
                                   uint32_t suggested,
                                   uint32_t buttonmap,
                                   neui_detail::DataItem* drop_item)
  {
    auto* s = static_cast<Session*>(session_ptr);
    if (!s) return 0;
    return s->dispatch_dnd_drop(frame_widget_id & 0xFFFFu,
                                 x, y, formats, formats_count,
                                 suggested, buttonmap, drop_item);
  }

  bool platform_dnd_register_window(void* native_handle, void* session_ptr,
                                     uint32_t frame_widget_id)
  {
    if (!native_handle) return false;
    HWND hwnd = static_cast<HWND>(native_handle);

    neui_detail::dnd_ensure_ole_initialized();

    neui_detail::DndDispatchSeam seam = {};
    seam.session_ptr     = session_ptr;
    seam.frame_widget_id = frame_widget_id;
    seam.on_enter        = &xpl_dnd_on_enter;
    seam.on_move         = &xpl_dnd_on_move;
    seam.on_leave        = &xpl_dnd_on_leave;
    seam.on_drop         = &xpl_dnd_on_drop;

    auto* target = new neui_detail::DropTargetImpl(hwnd, seam);
    HRESULT hr = RegisterDragDrop(hwnd, target);
    if (FAILED(hr)) {
      target->Release();
      return false;
    }
    // RegisterDragDrop AddRefs; release our local ref so the target is
    // owned solely by the OS until RevokeDragDrop.
    target->Release();
    return true;
  }

  void platform_dnd_unregister_window(void* native_handle)
  {
    if (!native_handle) return;
    RevokeDragDrop(static_cast<HWND>(native_handle));
  }

  void* platform_make_drag_preview(const uint8_t* bgra_premul,
                                     uint32_t w_px, uint32_t h_px,
                                     float /*scale*/)
  {
    return neui_detail::w32_make_drag_hbitmap(bgra_premul, w_px, h_px);
  }

  uint32_t platform_dnd_begin_drag(void* native_handle,
                                    neui_detail::DataItem* item,
                                    uint32_t allowed_actions,
                                    void* preview_native,
                                    int hot_x, int hot_y)
  {
    if (preview_native) {
      HBITMAP hbm = static_cast<HBITMAP>(preview_native);
      BITMAP bm = {};
      neui_detail::DragPreviewW32 preview;
      if (GetObjectW(hbm, sizeof(bm), &bm)) {
        preview.hbitmap = hbm;
        preview.width   = bm.bmWidth;
        preview.height  = (bm.bmHeight < 0) ? -bm.bmHeight : bm.bmHeight;
        preview.hot_x   = (hot_x < 0) ? preview.width  / 2 : hot_x;
        preview.hot_y   = (hot_y < 0) ? preview.height / 2 : hot_y;
        return neui_detail::platform_dnd_begin_drag_w32(native_handle, item,
                                                         allowed_actions,
                                                         &preview);
      }
      // GetObject failure: drop the bitmap and fall through to no-preview.
      DeleteObject(hbm);
    }
    return neui_detail::platform_dnd_begin_drag_w32(native_handle, item,
                                                     allowed_actions);
  }

  // -------------------------------------------------------------------------
  // Mouse cursor. Win32 cursor management is per-message: WM_SETCURSOR
  // fires every time the cursor moves over a window, and unless we set
  // the cursor ourselves the OS reverts to whatever the window class
  // registered. We track the active kind in a thread-local and use a
  // hook in WM_SETCURSOR to reapply it; for now we set immediately
  // (good enough for the GRID's drag-resize feedback, which is active
  // for the duration of the hover + drag).

  static int s_cursor_kind = NEUI_CURSOR_DEFAULT;

  void platform_set_cursor(int kind)
  {
    s_cursor_kind = kind;
    HCURSOR cur = nullptr;
    // IDC_* are LPCSTR resource ids; cast keeps them usable with the W
    // LoadCursor variant. The resource-id form ignores the char width.
    switch (kind) {
      case NEUI_CURSOR_EW_RESIZE: cur = LoadCursorW(nullptr, (LPCWSTR)IDC_SIZEWE); break;
      case NEUI_CURSOR_DEFAULT:
      default:                    cur = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);  break;
    }
    if (cur) SetCursor(cur);
  }

} // namespace xpl_host
