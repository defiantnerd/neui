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
#include "../shared/win32/keys_win32.h"
#include "../shared/win32/cursor_win32.h"
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
    // Set while WE are applying geometry (platform_set_window_pos,
    // platform_menubar_attach). The resulting WM_SIZE must not re-derive the
    // logical size from the physical one - `logical -> physical -> logical` is
    // lossy once a fractional zoom is involved - and must not report a RESIZE
    // the client just asked for. wd.width/height are already authoritative on
    // those paths (widget_set_size / the zoom handler set them first).
    bool     self_resizing     = false;
    WCHAR    pending_surrogate = 0;      // high surrogate waiting for its low pair
    // SMOOTH-scroll grid bounce timer: when a grid overscrolls in SMOOTH mode
    // we SetTimer the FRAME's HWND (grids on xpl have no HWND of their own)
    // and track which grid is bouncing so the WM_TIMER tick can find it.
    // Only one grid per frame is allowed to bounce at a time; a second
    // overscrolling grid replaces the first (rare edge case).
    uint32_t bouncing_grid_index = 0;
    // Same pattern for the scrolling SECTION's spring-back (kinetics live
    // in the section's SectionScrollState.kin_v/_h).
    uint32_t bouncing_section_index = 0;
  };

  // Timer ID for the grid spring-back animation on the frame's HWND.
  static constexpr UINT_PTR XPL_GRID_BOUNCE_TIMER_ID = 0x6E78676B;  // 'nxgk'
  // Timer ID for the scrolling-SECTION spring-back animation.
  static constexpr UINT_PTR XPL_SECTION_BOUNCE_TIMER_ID = 0x6E787363;  // 'nxsc'
  // Timer ID for the toast animation heartbeat (16 ms tick).
  static constexpr UINT_PTR XPL_TOAST_TIMER_ID = 0x6E78746F;  // 'nxto'

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

  // Convert a physical pixel coordinate to logical, for a FRAME - divides by
  // the frame's full logical->physical factor,
  // i.e. DPI ratio AND user zoom (NEUI_ATTR_UI_SCALE). Every mouse coordinate
  // and the WM_SIZE client size go through this, which is what keeps input in
  // the same logical space the widget tree and the paint walk use at any zoom.
  static inline float phys_to_log(int phys, const WidgetData& frame)
  {
    const float s = frame.logical_to_physical();
    return (s > 0.0f) ? static_cast<float>(phys) / s : static_cast<float>(phys);
  }

  // Logical -> physical for a frame (the inverse). Used where we hand native
  // APIs a pixel rect built from logical widget coords (IME caret, window
  // sizing).
  static inline int log_to_phys(float logical, const WidgetData& frame)
  {
    return static_cast<int>(logical * frame.logical_to_physical() + 0.5f);
  }

  static WindowUserData* get_wud(HWND hwnd)
  {
    return reinterpret_cast<WindowUserData*>(
      GetWindowLongPtrW(hwnd, GWLP_USERDATA));
  }

  // Count of live APPWINDOW instances; PostQuitMessage when it reaches 0.
  static int g_appwindow_count = 0;

  // -------------------------------------------------------------------------
  // Scrolling-SECTION kinetics helpers

  // Find the scrolling SECTION that owns the wheel at `hit`: the hit widget
  // itself, or its nearest ancestor exposing a SectionScrollState. 0 = none.
  static uint32_t find_scrolling_section(Session* s, uint32_t hit)
  {
    auto* hw = s->get_widget(hit);
    if (hw && hw->scroll_state_ptr()) return hit;
    auto parents = s->_widgets.get_all_parents(hit);
    for (uint32_t pidx : parents) {
      auto* pw = s->get_widget(pidx);
      if (pw && pw->scroll_state_ptr()) return pidx;
    }
    return 0;
  }

  // Feed a synthetic precise wheel delta into a scrolling SECTION's per-axis
  // kinetics - the SECTION twin of the GRID SMOOTH-mode branch, mirroring
  // the macOS sectionKineticWheel: (same axis fallback, same curve +
  // tuning). dv / dh are logical px with the kinetics' sign convention
  // (raw_px -= delta; positive dv = scroll up, positive dh = scroll left).
  // Starts the frame-HWND spring-back timer on overscroll release.
  // Respects NEUI_ATTR_SCROLL_KINETICS: STEPPED hard-clamps + skips bounce,
  // SMOOTH / PLATFORM-on-macOS-not-applicable-here (Win32 default = STEPPED).
  static void section_kinetic_wheel_w32(HWND hwnd, WindowUserData* wud,
                                         uint32_t sec_idx,
                                         double dv, double dh)
  {
    using namespace neui_detail;
    auto* sw = wud->session->get_widget(sec_idx);
    SectionScrollState* st = sw ? sw->scroll_state_ptr() : nullptr;
    const SectionLayout* L = sw ? sw->section_layout_ptr() : nullptr;
    if (!st || !L) return;

    bool has_v = section_axis_has_v(st->axis);
    bool has_h = section_axis_has_h(st->axis);
    // Asymmetric single-axis fallback. A horizontal-only section absorbs a
    // pure vertical wheel because classic wheel mice have no horizontal
    // axis and the user otherwise has no way to scroll it. A vertical-
    // only section does NOT absorb pure horizontal input: WM_MOUSEHWHEEL
    // (tilt wheels, precision-touchpad horizontal scroll), Shift+wheel,
    // and trackpad two-finger left/right all represent an explicit
    // horizontal-scroll intent that should simply be ignored when the
    // axis isn't supported, not silently re-aimed at the vertical axis.
    if (!has_v && has_h && dh == 0.0 && dv != 0.0) { dh = dv; dv = 0.0; }

    int  kin_mode = section_read_kinetics_mode(sw->attrs.get());
    bool smooth   = scroll_kinetics_smooth_enabled(kin_mode,
                                                    /*platform_default_smooth=*/false);

    bool changed = false;
    bool start_bounce = false;
    if (smooth) {
      ScrollWheelAction act_v{}, act_h{};
      if (has_v && dv != 0.0) {
        ScrollWheelInput in;
        in.precise  = true;   // px-true synthetic input; enables rubber-band
        in.delta_px = dv;
        act_v = section_scroll_wheel_kinetic(*st, *L, in, false);
      }
      if (has_h && dh != 0.0) {
        ScrollWheelInput in;
        in.precise  = true;
        in.delta_px = dh;
        act_h = section_scroll_wheel_kinetic(*st, *L, in, true);
      }
      changed      = act_v.changed      || act_h.changed;
      start_bounce = act_v.start_bounce || act_h.start_bounce;
    } else {
      if (has_v && dv != 0.0 && section_scroll_step_px(*st, *L, dv, false))
        changed = true;
      if (has_h && dh != 0.0 && section_scroll_step_px(*st, *L, dh, true))
        changed = true;
    }
    if (changed) {
      InvalidateRect(hwnd, nullptr, FALSE);
      sw->notify_scroll_changed();
    }
    if (start_bounce) {
      wud->bouncing_section_index = sec_idx;
      SetTimer(hwnd, XPL_SECTION_BOUNCE_TIMER_ID, 16, nullptr);
    }
  }

  // -------------------------------------------------------------------------
  // Keyboard helpers

  // Build a NEUI_KMOD_* bitmask from the current key state. The bits ARE the
  // public ones (<neui/d/keys.h>) - the old comment here claimed a private
  // convention that merely happened to have the same values, which would have
  // broken silently if the enum were ever reordered.
  static uint32_t build_modifiers()
  {
    return neui_detail::win32_kmod_from_state();
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
    // Via the frame's full factor (DPI ratio * zoom) - the caret is drawn
    // through the zoom transform, so the IME window has to follow it or the
    // candidate list detaches from the text at any zoom != 100%.
    int   px_x         = log_to_phys(lx_in_frame, *frame_wd);
    int   px_y         = log_to_phys(ly_in_frame, *frame_wd);
    int   px_h         = log_to_phys(lh,          *frame_wd);

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
  // Mouse cursor state.
  //
  // Declared HERE, above XplWndProc, because the WM_SETCURSOR case reads it -
  // and the rest of the cursor code lives ~1400 lines further down next to
  // platform_set_cursor. A definition down there would not be in scope at the
  // point of use, which is a hard compile error on MSVC (C2065) that this
  // macOS-only build cannot catch.
  //
  // Win32 cursor management is per-message: WM_SETCURSOR fires every time the
  // pointer moves over a window, and unless we answer it the OS reverts to
  // whatever the window class registered (wc.hCursor, an arrow). So the active
  // kind is tracked here and reapplied from the WM_SETCURSOR case;
  // platform_set_cursor also applies it immediately, for the case where the
  // pointer is already inside and no WM_SETCURSOR is due until it next moves.
  //
  // Process-wide rather than per-window: only one window can be under the
  // pointer at a time, and Session::refresh_cursor is the single writer.
  static int s_cursor_kind = NEUI_CURSOR_DEFAULT;

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
      // Give this window Win32 keyboard focus so WM_KEYDOWN/WM_CHAR arrive
      // here. NOT for a DAW-embedded frame (WS_CHILD): merely opening a
      // plugin editor must not yank focus out of whatever the host had
      // focused. An embedded frame takes focus on click instead
      // (WM_LBUTTONDOWN below), matching the macOS embedded path.
      if (!(static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)) & WS_CHILD))
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
            // Drop the modal pump so widget_show unwinds and returns.
            if (auto* fw = dynamic_cast<FrameWidget*>(wd))
              fw->modal_pump_active = false;
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

    case WM_ERASEBKGND: {
      // Fill the invalidated region with the frame's background colour (the
      // same colour D2D clears to) rather than leaving it black. The window
      // class has no background brush and D2D doesn't paint until WM_PAINT, so
      // without this any area exposed before the next D2D frame - most visibly
      // while resizing - flashes black. The erase DC is pre-clipped to the
      // update region, so a full-client FillRect only touches exposed pixels;
      // and because the fill colour equals the D2D clear colour, the eventual
      // repaint is seamless. We still return 1 (handled) so DefWindowProc
      // doesn't erase again with the (null) class brush.
      HDC  hdc = reinterpret_cast<HDC>(wParam);
      RECT rc; GetClientRect(hwnd, &rc);
      COLORREF col = GetSysColor(COLOR_BTNFACE);   // fallback before the frame exists
      auto* wud = get_wud(hwnd);
      if (wud && wud->session) {
        uint32_t argb = wud->session->frame_clear_color(wud->widget_index);
        col = RGB((argb >> 16) & 0xFF, (argb >> 8) & 0xFF, argb & 0xFF);
      }
      HBRUSH br = CreateSolidBrush(col);
      FillRect(hdc, &rc, br);
      DeleteObject(br);
      return 1;
    }

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

      // Constraints are LOGICAL, but they bound the ZOOMED native window, so
      // they need the same zoom factor the sizing paths use - otherwise a
      // MIN_WIDTH of 400 at zoom 2 lets the user drag down to logical 200.
      float mm_zoom = 1.0f;
      if (auto* zwud = get_wud(hwnd))
        if (zwud->session)
          if (auto* zfwd = zwud->session->get_widget(zwud->widget_index))
            mm_zoom = zfwd->ui_scale();
      if (min_w > 0) mmi->ptMinTrackSize.x = static_cast<int>(MulDiv(min_w, static_cast<int>(dpi), 96) * mm_zoom + 0.5f);
      if (min_h > 0) mmi->ptMinTrackSize.y = static_cast<int>(MulDiv(min_h, static_cast<int>(dpi), 96) * mm_zoom + 0.5f);
      if (max_w > 0 && (min_w <= 0 || max_w >= min_w))
        mmi->ptMaxTrackSize.x = static_cast<int>(MulDiv(max_w, static_cast<int>(dpi), 96) * mm_zoom + 0.5f);
      if (max_h > 0 && (min_h <= 0 || max_h >= min_h))
        mmi->ptMaxTrackSize.y = static_cast<int>(MulDiv(max_h, static_cast<int>(dpi), 96) * mm_zoom + 0.5f);
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
        // Our own geometry application: wd.width/height are authoritative and
        // the client did not "get resized". Skipping also avoids the lossy
        // round-trip below - truncation used to drift a 401-wide frame at zoom
        // 1.25 down to 400 (501 physical / 1.25 = 400.8 -> 400).
        if (fwd && !wud->self_resizing) {
          // Round, don't truncate: phys_to_log divides by (dpi/96 * zoom), so
          // truncation loses up to a pixel on every externally-driven resize.
          int w_log = static_cast<int>(phys_to_log(static_cast<int>(w_phys), *fwd) + 0.5f);
          int h_log = static_cast<int>(phys_to_log(static_cast<int>(h_phys), *fwd) + 0.5f);
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
    // Cursor. Win32 cursor state is per-message, not per-window: the window
    // class registers an arrow in wc.hCursor and DefWindowProc reapplies it on
    // EVERY WM_SETCURSOR, which the OS sends on every pointer move over the
    // window. Without this handler a cursor set from platform_set_cursor is
    // reverted before the next frame - the GRID's column-resize cursor only
    // ever "worked" because WM_MOUSEMOVE re-set it immediately afterwards, so
    // it flickered and it never stuck when the pointer stopped moving.
    //
    // Only the CLIENT area is ours. Handing back the non-client hit-test codes
    // to DefWindowProc keeps the OS's own resize-border and title-bar cursors,
    // which a client would otherwise lose the moment it set any cursor.
    case WM_SETCURSOR: {
      if (LOWORD(lParam) != HTCLIENT) break;   // -> DefWindowProc
      neui_detail::win32_apply_cursor(s_cursor_kind);
      return TRUE;                             // "I handled it, don't reset"
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

      float lx = phys_to_log(mouse_x(lParam), *fwd);
      float ly = phys_to_log(mouse_y(lParam), *fwd);

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
          // Was masked to the three button bits only, which dropped MK_SHIFT /
          // MK_CONTROL on every MOUSE_MOVE - so Shift-for-fine on a KNOB /
          // SLIDER drag saw the modifier on the initial DOWN and then lost it
          // for the rest of the drag. The shared helper keeps all five
          // documented bits (and, unlike a raw wParam forward, drops
          // MK_XBUTTON1 = 0x0020, which would otherwise read as NEUI_MK_ALT).
          ev.data.mouse.buttonmap = neui_detail::win32_buttonmap(wParam);
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

      // Click-to-focus for a DAW-embedded frame. Windows focuses a top-level
      // window on click by itself, but never a WS_CHILD - so without this,
      // once focus moves to the host's UI it never comes back and keyboard
      // input to an embedded editor is dead. Mirrors the macOS embedded
      // path's makeFirstResponder: in NEUIView mouseDown:.
      if ((static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE)) & WS_CHILD)
          && GetFocus() != hwnd)
        SetFocus(hwnd);

      float lx = phys_to_log(mouse_x(lParam), *fwd);
      float ly = phys_to_log(mouse_y(lParam), *fwd);

      // Popup menu overlay absorbs the click - picks an item or dismisses.
      // The nested message pump in open_popup_menu will see _popup_running
      // flip to false and return.
      if (wud->session->_popup_active) {
        wud->session->handle_popup_click(lx, ly);
        return 0;
      }

      // Toast overlay absorbs a click that lands on its rect, jumping it
      // to the fade-out phase. Clicks outside the toast rect fall through.
      if (wud->session->handle_toast_click(wud->widget_index, lx, ly))
        return 0;

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
          ev.data.mouse.buttonmap = neui_detail::win32_buttonmap(wParam);
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

      float lx = phys_to_log(mouse_x(lParam), *fwd);
      float ly = phys_to_log(mouse_y(lParam), *fwd);

      uint32_t hit = wud->session->widget_at(lx, ly, wud->widget_index);

      // The OS replaces the second press of a rapid pair with DBLCLK (the
      // window class carries CS_DBLCLKS for tree / inputbox / grid). Mirror
      // the DOWN setup so the following UP still produces a CLICK on widgets
      // that don't opt into DBLCLICK (BUTTON in particular) - this is what
      // gives parity with the native win32 host, where the system "Button"
      // class has no CS_DBLCLKS and every fast click is a plain click.
      wud->session->set_focus(hit);
      wud->session->set_pressed(hit);
      SetCapture(hwnd);

      if (hit != 0) {
        auto* hw = wud->session->get_widget(hit);
        if (hw) {
          neui_event_t ev = {};
          ev.type                 = NEUI_EVENT_MOUSE_BUTTON_DBLCLICK;
          ev.data.mouse.widget    = { hw->widget_id };
          ev.data.mouse.x         = static_cast<int>(lx);
          ev.data.mouse.y         = static_cast<int>(ly);
          ev.data.mouse.buttonmap = neui_detail::win32_buttonmap(wParam);
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

      float lx = phys_to_log(mouse_x(lParam), *fwd);
      float ly = phys_to_log(mouse_y(lParam), *fwd);

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
          ev.data.mouse.buttonmap = neui_detail::win32_buttonmap(wParam);

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
      float lx = phys_to_log(mouse_x(lParam), *fwd);
      float ly = phys_to_log(mouse_y(lParam), *fwd);
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
      ev.data.mouse.buttonmap = neui_detail::win32_buttonmap(wParam);
      wud->session->dispatch_mouse_event(hit, &ev);
      return 0;
    }

    case WM_RBUTTONUP: {
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      auto* fwd = wud->session->get_widget(wud->widget_index);
      if (!fwd) break;
      float lx = phys_to_log(mouse_x(lParam), *fwd);
      float ly = phys_to_log(mouse_y(lParam), *fwd);
      uint32_t hit = wud->session->widget_at(lx, ly, wud->widget_index);
      if (hit == 0) return 0;
      auto* hw = wud->session->get_widget(hit);
      if (!hw) return 0;
      neui_event_t ev = {};
      ev.type                 = NEUI_EVENT_MOUSE_RBUTTON_UP;
      ev.data.mouse.widget    = { hw->widget_id };
      ev.data.mouse.x         = static_cast<int>(lx);
      ev.data.mouse.y         = static_cast<int>(ly);
      ev.data.mouse.buttonmap = neui_detail::win32_buttonmap(wParam);
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
      float lx = phys_to_log(pt.x, *fwd);
      float ly = phys_to_log(pt.y, *fwd);

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
      // Shift + vertical wheel = conventional horizontal-scroll mouse
      // fallback. Flip the sign so positive delta = scroll-left, matching
      // the WM_MOUSEHWHEEL convention.
      bool shift_held = (wParam & MK_SHIFT) != 0;
      ev.data.wheel.delta         = shift_held ? -delta : delta;
      ev.data.wheel.is_horizontal = shift_held ? 1 : 0;
      // The wheel messages pack the key/button state in the LOW word (the
      // HIGH word is the delta), so unlike the mouse messages this needs
      // GET_KEYSTATE_WPARAM first. Masking matters: the low word can carry
      // MK_XBUTTON1 = 0x0020, which collides with NEUI_MK_ALT.
      ev.data.wheel.buttonmap     = neui_detail::win32_buttonmap(GET_KEYSTATE_WPARAM(wParam));

      // Scrolling SECTION (the hit itself or its nearest ancestor): widgets
      // below the section get first refusal via a bounded bubble; when
      // nothing below consumes, the section eats the wheel through its
      // kinetics (pixel-precise + elastic rubber-band - identical dynamics
      // to the GRID SMOOTH mode).
      uint32_t sec_idx = find_scrolling_section(wud->session, hit);
      if (sec_idx != 0) {
        if (hit != sec_idx &&
            wud->session->dispatch_wheel_event(hit, &ev, sec_idx))
          return 0;
        double notches = (double)raw_delta / (double)WHEEL_DELTA;
        double px = notches * (double)scroll_lines
                            * neui_detail::SECTION_WHEEL_LINE_PX;
        // Shift routes the delta to the horizontal axis; sign flipped to
        // match the line path's "wheel-up = scroll-right" convention.
        if (shift_held)
          section_kinetic_wheel_w32(hwnd, wud, sec_idx, 0.0, -px);
        else
          section_kinetic_wheel_w32(hwnd, wud, sec_idx, px, 0.0);
        return 0;
      }

      // Wheel bubbles up to ancestors so a scrolling SECTION consumes the
      // wheel when the inner widget under the cursor doesn't.
      wud->session->dispatch_wheel_event(hit, &ev);
      return 0;
    }

    case WM_MOUSEHWHEEL: {
      // Trackpad two-finger horizontal scroll. Same payload shape as
      // WM_MOUSEWHEEL except wParam's delta is the horizontal axis and
      // positive = right. Sign-flip so positive delta = scroll-left,
      // matching the vertical wheel's "positive = scroll-up" convention.
      auto* wud = get_wud(hwnd);
      if (!wud) break;
      auto* fwd = wud->session->get_widget(wud->widget_index);
      if (!fwd) break;

      POINT pt = { mouse_x(lParam), mouse_y(lParam) };
      ScreenToClient(hwnd, &pt);
      float lx = phys_to_log(pt.x, *fwd);
      float ly = phys_to_log(pt.y, *fwd);

      int raw_delta = GET_WHEEL_DELTA_WPARAM(wParam);
      UINT scroll_chars = 3;
      SystemParametersInfoW(SPI_GETWHEELSCROLLCHARS, 0, &scroll_chars, 0);
      int delta = -(raw_delta * static_cast<int>(scroll_chars)) / WHEEL_DELTA;

      uint32_t hit = wud->session->widget_at(lx, ly, wud->widget_index);
      if (hit == 0) break;
      auto* hw = wud->session->get_widget(hit);
      if (!hw) break;

      neui_event_t ev = {};
      ev.type                     = NEUI_EVENT_MOUSE_WHEEL;
      ev.data.wheel.widget        = { hw->widget_id };
      ev.data.wheel.x             = static_cast<int>(lx);
      ev.data.wheel.y             = static_cast<int>(ly);
      ev.data.wheel.delta         = delta;
      ev.data.wheel.is_horizontal = 1;
      ev.data.wheel.buttonmap     = neui_detail::win32_buttonmap(GET_KEYSTATE_WPARAM(wParam));

      // Scrolling SECTION: bounded bubble below, kinetics on the section -
      // same shape as the WM_MOUSEWHEEL branch above.
      uint32_t sec_idx = find_scrolling_section(wud->session, hit);
      if (sec_idx != 0) {
        if (hit != sec_idx &&
            wud->session->dispatch_wheel_event(hit, &ev, sec_idx))
          return 0;
        double notches = (double)raw_delta / (double)WHEEL_DELTA;
        // Tilt-right (positive raw) = scroll right; the kinetics sign
        // convention (raw_px -= delta) wants negative for scroll-right.
        double px = -notches * (double)scroll_chars
                             * neui_detail::SECTION_WHEEL_LINE_PX;
        section_kinetic_wheel_w32(hwnd, wud, sec_idx, 0.0, px);
        return 0;
      }

      wud->session->dispatch_wheel_event(hit, &ev);
      return 0;
    }

    case WM_TIMER: {
      // Toast animation tick: just invalidate the frame; paint_toast self-
      // terminates when the toast lifetime expires (paint_toast clears
      // toast.active and calls platform_stop_toast_animation, which the
      // ondemand-frame post-paint pass picks up next tick).
      if (wParam == XPL_TOAST_TIMER_ID) {
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
      }
      // Scrolling-SECTION spring-back tick - same shape as the grid timer
      // below; both axes step in one tick (a "both" section can overscroll
      // vertically and horizontally in the same gesture).
      if (wParam == XPL_SECTION_BOUNCE_TIMER_ID) {
        auto* swud = get_wud(hwnd);
        if (!swud) { KillTimer(hwnd, XPL_SECTION_BOUNCE_TIMER_ID); return 0; }
        auto* shw = (swud->bouncing_section_index != 0)
                      ? swud->session->get_widget(swud->bouncing_section_index)
                      : nullptr;
        neui_detail::SectionScrollState* st =
          shw ? shw->scroll_state_ptr() : nullptr;
        const neui_detail::SectionLayout* L =
          shw ? shw->section_layout_ptr() : nullptr;
        if (!st || !L) {
          swud->bouncing_section_index = 0;
          KillTimer(hwnd, XPL_SECTION_BOUNCE_TIMER_ID);
          return 0;
        }
        bool more_v = neui_detail::section_scroll_bounce_step(*st, *L, false);
        bool more_h = neui_detail::section_scroll_bounce_step(*st, *L, true);
        InvalidateRect(hwnd, nullptr, FALSE);
        shw->notify_scroll_changed();
        if (!more_v && !more_h) {
          swud->bouncing_section_index = 0;
          KillTimer(hwnd, XPL_SECTION_BOUNCE_TIMER_ID);
        }
        return 0;
      }
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

    // neui's create() width/height specify the CLIENT (content) area - the same
    // contract as the win32 native host (hosts/win32/widgets.cpp) and macOS
    // (initWithContentRect:), and what get_client_rect reports back.
    // CreateWindowExW takes the OUTER window size, so grow the requested client
    // rect by the non-client frame (title bar, resize borders, and the menu-bar
    // row when this frame carries a menubar) via AdjustWindowRectExForDpi.
    // Without this the usable client is ~30-50 px shorter and a few px narrower
    // than asked for, clipping any widget laid out flush to the right / bottom
    // edge. The menubar is SetMenu'd onto the frame right after creation (see
    // widget show in widgets.cpp), so reserve its row here too.
    bool has_menu = false;
    for (uint32_t ci = session->_widgets.child(widget_index); ci != 0;
         ci = session->_widgets.next(ci)) {
      if (!session->_widgets.exists(ci)) continue;
      if (!session->_widgets[ci].is_menubar()) continue;
      auto* mb = dynamic_cast<MenubarWidget*>(&session->_widgets[ci]);
      if (mb && mb->hmenu) { has_menu = true; break; }
    }

    // The frame's user zoom scales the CLIENT area only - the non-client
    // chrome (title bar, borders, menu row) is OS-drawn at the monitor's DPI
    // and must not be zoomed, which is why the zoom multiplies before
    // AdjustWindowRectExForDpi rather than after.
    const float zoom = wd.ui_scale();
    auto outer_for_dpi = [&](UINT dpi) -> SIZE {
      RECT wr = { 0, 0,
                  static_cast<int>(MulDiv(wd.width,  static_cast<int>(dpi), 96) * zoom + 0.5f),
                  static_cast<int>(MulDiv(wd.height, static_cast<int>(dpi), 96) * zoom + 0.5f) };
      AdjustWindowRectExForDpi(&wr, style, has_menu ? TRUE : FALSE, ex_style, dpi);
      return SIZE{ wr.right - wr.left, wr.bottom - wr.top };
    };
    SIZE outer = outer_for_dpi(sys_dpi);

    auto* wud = new WindowUserData{ session, widget_index };

    HWND hwnd = CreateWindowExW(
      ex_style,
      k_wndclass,
      L"",
      style,
      MulDiv(wd.x,      sys_dpi, 96),
      MulDiv(wd.y,      sys_dpi, 96),
      outer.cx,
      outer.cy,
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
      SIZE outer2 = outer_for_dpi(wd.dpi);
      SetWindowPos(hwnd, nullptr, 0, 0,
                   outer2.cx, outer2.cy,
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
    // wd.embed_parent (set via platform_set_embed_parent) selects the
    // DAW-embedded path: a WS_CHILD of the foreign parent HWND. The DAW's
    // own message pump then delivers WM_PAINT / input / WM_TIMER to the
    // child - neui owns no loop in embedded mode. AdjustWindowRectExForDpi
    // is an identity for WS_CHILD, so the client-area contract holds
    // unchanged, and (x, y) is parent-client-relative.
    if (wd.embed_parent) {
      create_native_window(session, widget_index, wd,
        WS_CHILD | WS_CLIPSIBLINGS | WS_CLIPCHILDREN,
        0,
        reinterpret_cast<HWND>(wd.embed_parent));
      return;
    }
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
    // A DAW-embedded child never steals foreground from its host - focus
    // ownership stays with the DAW's top-level.
    if (GetWindowLongPtrW(h, GWL_STYLE) & WS_CHILD) return;
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
    HWND hwnd = static_cast<HWND>(native_handle);
    if (!hwnd) return;
    if (dpi == 0) dpi = 96;
    // w/h are the logical (96 DPI base) CLIENT size - this seam is frame-only
    // in the xpl host (child widgets are painted, not HWNDs), so grow the
    // requested client rect to the OUTER window size the same way
    // create_native_window does, otherwise a programmatic set_size shrinks the
    // usable client by the title bar / border / menu chrome. Style, ex-style
    // and menu presence are read back from the live HWND. Position (x, y) is
    // kept as the outer top-left, matching the create path.
    // The client area is additionally scaled by the frame's user zoom (the
    // chrome is not - see create_native_window). The zoom is read off the
    // frame widget via the HWND's user data, since this seam only gets a
    // native handle.
    float zoom = 1.0f;
    if (auto* wud = get_wud(hwnd)) {
      if (wud->session) {
        if (auto* fwd = wud->session->get_widget(wud->widget_index))
          zoom = fwd->ui_scale();
      }
    }
    RECT wr = { 0, 0,
                static_cast<int>(MulDiv(w, static_cast<int>(dpi), 96) * zoom + 0.5f),
                static_cast<int>(MulDiv(h, static_cast<int>(dpi), 96) * zoom + 0.5f) };
    DWORD style    = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    BOOL  has_menu = GetMenu(hwnd) != nullptr;
    AdjustWindowRectExForDpi(&wr, style, has_menu, ex_style, dpi);
    // Bracket the call so the resulting WM_SIZE knows this resize is ours (see
    // WindowUserData::self_resizing). SetWindowPos dispatches WM_SIZE
    // synchronously, so the flag is live for exactly the right window.
    auto* wud_flag = get_wud(hwnd);
    if (wud_flag) wud_flag->self_resizing = true;
    const bool keep_pos = (x == NEUI_WINDOW_POS_KEEP || y == NEUI_WINDOW_POS_KEEP);
    SetWindowPos(hwnd, nullptr,
      keep_pos ? 0 : MulDiv(x, static_cast<int>(dpi), 96),
      keep_pos ? 0 : MulDiv(y, static_cast<int>(dpi), 96),
      wr.right - wr.left,
      wr.bottom - wr.top,
      SWP_NOZORDER | SWP_NOACTIVATE | (keep_pos ? SWP_NOMOVE : 0u));
    if (wud_flag) wud_flag->self_resizing = false;
  }

  void platform_post_close(void* native_handle)
  {
    PostMessageW(static_cast<HWND>(native_handle), WM_CLOSE, 0, 0);
  }

  // ---- DAW-embedding seams. -------------------------------------------------
  // On Win32 an embedded PLUGWINDOW is an ordinary WS_CHILD HWND, so the
  // DAW's message pump already delivers WM_PAINT / input / WM_TIMER (bounce,
  // toast) to it - there is no dedicated connection to poll and nothing to
  // tick. The seams exist so a plugin adapter can drive one platform-uniform
  // loop across Win32 / macOS / Linux.

  void platform_set_embed_parent(Session* session, uint32_t widget_index,
                                 void* native_parent)
  {
    if (!session) return;
    auto* wd = session->get_widget(widget_index);
    if (wd) wd->embed_parent = reinterpret_cast<uintptr_t>(native_parent);
  }

  int platform_embed_event_fd(void* /*native_handle*/) { return -1; }

  void platform_embed_pump_and_tick(void* /*native_handle*/) {}

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

  bool platform_menubar_in_frame() { return false; }   // Win32 uses a native HMENU
  int  platform_frame_extra_top_inset(void* /*nh*/, bool /*has_menubar*/) { return 0; }

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
    HWND hwnd = static_cast<HWND>(frame_hwnd);
    SetMenu(hwnd, static_cast<HMENU>(hmenu));
    DrawMenuBar(hwnd);

    // Adding a menu bar consumes a row from the client area, but the frame's
    // create() size is the CLIENT area - so restore it: recompute the outer
    // window size for the stored logical client dims *with* the menu row and
    // resize to it. This is idempotent by design:
    //   - menubar present at create time: create_native_window already sized
    //     the window with the menu allowance, so the target equals the current
    //     outer size and this is a no-op.
    //   - menubar created after the frame is shown (dynamic): the window was
    //     sized without a menu row, so SetMenu above stole a client row; this
    //     grows the window back so the client stays the requested size.
    WindowUserData* wud = get_wud(hwnd);
    if (!wud || !wud->session) return;
    if (!wud->session->_widgets.exists(wud->widget_index)) return;
    auto& wd = wud->session->_widgets[wud->widget_index];
    UINT dpi = wd.dpi ? wd.dpi : GetDpiForWindow(hwnd);
    if (dpi == 0) dpi = 96;
    // MUST include the frame zoom, exactly like create_native_window and
    // platform_set_window_pos. Without it, attaching a menubar to a zoomed
    // frame resizes the window back to the UNZOOMED client size, and the
    // resulting WM_SIZE then divides that by the full factor - so a 400-wide
    // frame at zoom 2 ends up storing wd.width = 200 and the layout is
    // destroyed, not merely mis-sized. This runs at widget_show for any frame
    // with a MENUBAR child and again on every menubar rebuild.
    const float zoom = wd.ui_scale();
    RECT wr = { 0, 0,
                static_cast<int>(MulDiv(wd.width,  static_cast<int>(dpi), 96) * zoom + 0.5f),
                static_cast<int>(MulDiv(wd.height, static_cast<int>(dpi), 96) * zoom + 0.5f) };
    DWORD style    = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_STYLE));
    DWORD ex_style = static_cast<DWORD>(GetWindowLongPtrW(hwnd, GWL_EXSTYLE));
    AdjustWindowRectExForDpi(&wr, style, TRUE, ex_style, dpi);
    wud->self_resizing = true;
    SetWindowPos(hwnd, nullptr, 0, 0,
                 wr.right - wr.left, wr.bottom - wr.top,
                 SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    wud->self_resizing = false;
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
    // Submenus are matched by their HMENU value used as the MF_BYCOMMAND id;
    // RemoveMenu's position arg is UINT, so take the low 32 bits explicitly
    // (the same truncation the insert path applied, so it round-trips).
    RemoveMenu(static_cast<HMENU>(parent_hmenu),
               static_cast<UINT>(reinterpret_cast<UINT_PTR>(static_cast<HMENU>(submenu))),
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

  void platform_menubar_check_item(void* parent_hmenu, uint32_t cmd_id, bool checked)
  {
    if (!parent_hmenu) return;
    CheckMenuItem(static_cast<HMENU>(parent_hmenu), cmd_id,
                  MF_BYCOMMAND | (checked ? MF_CHECKED : MF_UNCHECKED));
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

  // No PRIMARY selection on Win32.
  void platform_clipboard_set_primary(const char* /*utf8*/, uint32_t /*length*/) {}
  int  platform_clipboard_get_primary(char* /*buf*/, int /*buflen*/) { return 0; }

  bool platform_clipboard_write_item(const neui_detail::DataItem& item)
  {
    return neui_detail::clipboard_write_item_win32(item);
  }

  bool platform_clipboard_read_item(neui_detail::DataItem& item)
  {
    return neui_detail::clipboard_read_item_win32(item);
  }

  // -------------------------------------------------------------------------
  // Drag & drop. The dispatch seam is built by the shared
  // make_dnd_dispatch_seam (hosts/shared/win32/dnd_target_win32.h), whose
  // callbacks forward into Session::dispatch_dnd_* - each converts to a
  // NEUI_EVENT_DND_* and walks the widget tree for the deepest
  // drop_target match.

  bool platform_dnd_register_window(void* native_handle, void* session_ptr,
                                     uint32_t frame_widget_id)
  {
    if (!native_handle) return false;
    HWND hwnd = static_cast<HWND>(native_handle);

    neui_detail::dnd_ensure_ole_initialized();

    neui_detail::DndDispatchSeam seam = neui_detail::make_dnd_dispatch_seam(
      static_cast<Session*>(session_ptr), frame_widget_id);

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
  // Client timers (NEUI_API_TIMER). A THREAD timer - SetTimer(NULL, ...) with a
  // TIMERPROC - rather than a window timer, because timers are session-scoped
  // and a session may own several frames (or, embedded, a frame whose lifetime
  // the DAW controls).
  //
  // The payoff is that one mechanism covers all three loops: WM_TIMER with a
  // NULL hwnd lands on the THREAD queue, so PeekMessage/GetMessage picks it up
  // and DispatchMessageW invokes the TIMERPROC - which is exactly what
  // platform_run, platform_pump_once, AND a DAW's own pump all do. No extra
  // plumbing per loop.
  static std::unordered_map<UINT_PTR, Session*>& w32_session_timers()
  {
    // Immortal: ~Session (global `sessions` in host.cpp, destroyed last) calls
    // platform_timer_stop during static teardown, after this TU's statics would
    // already be gone.
    static auto* m = new std::unordered_map<UINT_PTR, Session*>();
    return *m;
  }

  static void CALLBACK w32_client_timer_proc(HWND, UINT, UINT_PTR id, DWORD)
  {
    auto& m = w32_session_timers();
    auto it = m.find(id);
    // Re-check membership: a session torn down between fires would otherwise
    // be a dangling pointer.
    if (it == m.end() || !it->second) return;
    it->second->tick_client_timers();
  }

  // win32: phys_to_log divides every input coord by the zoom.
  bool platform_supports_ui_scale() { return true; }

  void platform_timer_start(Session* session, uint32_t interval_ms)
  {
    if (!session || interval_ms == 0) return;
    platform_timer_stop(session);   // idempotent re-arm at the new interval
    // id 0 asks the system to allocate one; the return value is the real id.
    // Note SetTimer clamps interval_ms up to USER_TIMER_MINIMUM (10 ms), so a
    // 4..9 ms request ticks at ~10 ms here - within the API's documented
    // "interval is a minimum" contract, and called out in <neui/d/timer.h>.
    UINT_PTR id = SetTimer(nullptr, 0, interval_ms, w32_client_timer_proc);
    if (id) w32_session_timers()[id] = session;
    // On failure leave nothing registered; Session::sync_timer_tick's cached
    // interval would otherwise claim "armed" and the timers would be silently
    // dead. Clearing the cache makes the next add_timer retry the arm.
    else if (session) session->notify_timer_arm_failed();
  }

  void platform_timer_stop(Session* session)
  {
    if (!session) return;
    auto& m = w32_session_timers();
    for (auto it = m.begin(); it != m.end(); ) {
      if (it->second == session) { KillTimer(nullptr, it->first); it = m.erase(it); }
      else                        ++it;
    }
  }

  void platform_set_cursor(int kind)
  {
    s_cursor_kind = kind;
    // Apply now for the common case (the pointer is already inside the client
    // area, so no WM_SETCURSOR is coming until it moves again), and let the
    // WM_SETCURSOR handler reapply it from then on. The kind->HCURSOR table is
    // shared with the native win32 host: ../shared/win32/cursor_win32.h.
    neui_detail::win32_apply_cursor(kind);
  }

  // -------------------------------------------------------------------------
  // Toast animation heartbeat.

  void platform_start_toast_animation(void* native_handle)
  {
    if (!native_handle) return;
    SetTimer(static_cast<HWND>(native_handle), XPL_TOAST_TIMER_ID, 16, nullptr);
  }

  void platform_stop_toast_animation(void* native_handle)
  {
    if (!native_handle) return;
    KillTimer(static_cast<HWND>(native_handle), XPL_TOAST_TIMER_ID);
  }

  uint64_t platform_now_ms()
  {
    return static_cast<uint64_t>(GetTickCount64());
  }

  // -------------------------------------------------------------------------
  // Modal message box - NEUI_MB_* values match MB_* numerically, so this
  // is a sanitize-mask + MessageBoxExW pass-through. The OS handles owner
  // disabling, focus return, and Esc/Enter for us.

  int platform_message_box(void* native_handle, const char* text,
                           const char* caption, uint32_t flags)
  {
    if (!native_handle) return 0;
    UINT mb = flags & (MB_TYPEMASK | MB_ICONMASK | MB_DEFMASK);
    if ((mb & MB_TYPEMASK) > MB_CANCELTRYCONTINUE) mb &= ~MB_TYPEMASK;
    std::wstring wtext    = to_wide(text);
    std::wstring wcaption = to_wide(caption);
    return MessageBoxExW(static_cast<HWND>(native_handle),
                         wtext.c_str(),
                         caption ? wcaption.c_str() : nullptr,
                         mb, MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL));
  }

} // namespace xpl_host
