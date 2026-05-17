#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <neui/neui.h>
#include <cstring>
#include <memory>
#include <vector>
#include <algorithm>
#include <string>
#include "window.h"  // provides get_hinstance(), ChildSubclassProc
#include "../../backends/d2d/d2d_backend.h"
#include "../shared/win32/clipboard_win32.h"
#include "../shared/win32/icon_win32.h"
#include "../shared/win32/image_loader_win32.h"
#include "../shared/win32/accel_table_win32.h"
#include "../shared/shortcut_format.h"
#include "../shared/widget_paint_knob.h"
#include "../shared/widget_paint_section.h"
#include "../shared/theme_palette.h"
#include <commctrl.h>

namespace win32_host
{
  extern std::vector<std::unique_ptr<Session>> sessions;
  bool run();

  // Widget id layout: upper 16 = owning session id, lower 16 = tree slot.
  // The lower-16 slot index is what the Tree<> uses; the upper 16 lets the
  // boundary validate that a handle from session A isn't being applied to
  // session B (relevant for audio plugins where many sessions live in one
  // process). Internal Session methods assume the widget belongs to them
  // and only mask the lower 16; the boundary helpers below enforce that.

  static uint32_t WidgetToIndex(neui_widget_t widget)
  {
    return widget.id & 0xffff;
  }

  // Forward declarations used by helpers higher up in this file.
  static int NEUI_ABI popup_menu(neui_session_t session, neui_widget_t anchor,
                                  int x, int y, const char* const* items);

  neui_widget_t IndexToWidget(uint32_t session_id, uint32_t idx)
  {
    return { ((session_id & 0xffff) << 16) | (idx & 0xffff) };
  }

  static Session* get_session(neui_session_t session)
  {
    uint32_t idx = (session.session & 0xffff) - 1;
    if (idx < sessions.size()) {
      return sessions[idx].get();
    }
    return nullptr;
  }

  // True if `widget` is either a sentinel (widget_root / widget_none) or a
  // properly-packed handle whose session id matches `session_id`.
  static bool widget_belongs_to_session(neui_widget_t widget, uint32_t session_id)
  {
    if (widget.id == 0)          return true;   // widget_root
    if (widget.id == UINT32_MAX) return true;   // widget_none
    return ((widget.id >> 16) & 0xffff) == (session_id & 0xffff);
  }

  // Resolve the session for an API call that operates on a specific widget.
  // Returns nullptr if the session is invalid OR the widget belongs to a
  // different session - the call is silently dropped.
  static Session* get_session_for_widget(neui_session_t session, neui_widget_t widget)
  {
    auto* s = get_session(session);
    if (!s) return nullptr;
    if (!widget_belongs_to_session(widget, s->session_id())) return nullptr;
    return s;
  }

  static int LogicalToPhysical(int logical, UINT dpi)
  {
    return MulDiv(logical, (int)dpi, 96);
  }

  // Widget type → Win32 class mapping
  struct WidgetTypeInfo {
    const char* neui_type;
    LPCWSTR win32_class;
    DWORD style;
    bool needs_subclass;
  };

  static const WidgetTypeInfo widget_type_table[] = {
    { NEUI_W_LABEL,     L"Static",  WS_CHILD | SS_LEFT,                                                         false },
    { NEUI_W_BUTTON,    L"Button",  WS_CHILD | WS_TABSTOP | BS_PUSHBUTTON,                                      true  },
    { NEUI_W_INPUTBOX,  L"Edit",    WS_CHILD | WS_TABSTOP | WS_BORDER | ES_LEFT | ES_AUTOHSCROLL,               true  },
    { NEUI_W_CHECKBOX,  L"Button",  WS_CHILD | WS_TABSTOP | BS_AUTOCHECKBOX,                                    true  },
    { NEUI_W_CHECKBOX3, L"Button",  WS_CHILD | WS_TABSTOP | BS_AUTO3STATE,                                       true  },
    { NEUI_W_LISTBOX,   L"ListBox", WS_CHILD | WS_TABSTOP | WS_BORDER | WS_VSCROLL | LBS_NOTIFY | LBS_HASSTRINGS, true },
    { NEUI_W_COMBOBOX,  L"ComboBox",WS_CHILD | WS_TABSTOP | CBS_DROPDOWNLIST | CBS_HASSTRINGS,                  true  },
    { NEUI_W_MULTILINE, L"Edit",    WS_CHILD | WS_TABSTOP | WS_BORDER | ES_MULTILINE | ES_AUTOVSCROLL | WS_VSCROLL, false },
    { NEUI_W_TREEVIEW,  L"SysTreeView32",
      WS_CHILD | WS_TABSTOP | WS_BORDER | TVS_HASLINES | TVS_LINESATROOT | TVS_HASBUTTONS | TVS_SHOWSELALWAYS, true },
    { NEUI_W_SLIDER,    TRACKBAR_CLASSW,
      WS_CHILD | WS_TABSTOP | TBS_HORZ | TBS_NOTICKS,                                                          true },
  };

  // Message sets for listbox vs combobox, selected by widget type.
  struct ItemsOps {
    UINT add;       // LB_ADDSTRING    / CB_ADDSTRING
    UINT insert;    // LB_INSERTSTRING / CB_INSERTSTRING
    UINT del;       // LB_DELETESTRING / CB_DELETESTRING
    UINT clear;     // LB_RESETCONTENT / CB_RESETCONTENT
    UINT count;     // LB_GETCOUNT     / CB_GETCOUNT
    UINT textlen;   // LB_GETTEXTLEN   / CB_GETLBTEXTLEN
    UINT gettext;   // LB_GETTEXT      / CB_GETLBTEXT
    UINT setdata;   // LB_SETITEMDATA  / CB_SETITEMDATA
    UINT getdata;   // LB_GETITEMDATA  / CB_GETITEMDATA
    UINT getsel;    // LB_GETCURSEL    / CB_GETCURSEL
    UINT setsel;    // LB_SETCURSEL    / CB_SETCURSEL
    UINT desel;     // deselect sentinel value for setsel
  };

  static const ItemsOps listbox_ops  = { LB_ADDSTRING, LB_INSERTSTRING, LB_DELETESTRING, LB_RESETCONTENT, LB_GETCOUNT, LB_GETTEXTLEN,   LB_GETTEXT,   LB_SETITEMDATA, LB_GETITEMDATA, LB_GETCURSEL, LB_SETCURSEL, (UINT)-1 };
  static const ItemsOps combobox_ops = { CB_ADDSTRING, CB_INSERTSTRING, CB_DELETESTRING, CB_RESETCONTENT, CB_GETCOUNT, CB_GETLBTEXTLEN, CB_GETLBTEXT, CB_SETITEMDATA, CB_GETITEMDATA, CB_GETCURSEL, CB_SETCURSEL, (UINT)-1 };

  static const ItemsOps* get_items_ops(const char* type) {
    if (type && !strcmp(type, NEUI_W_LISTBOX))  return &listbox_ops;
    if (type && !strcmp(type, NEUI_W_COMBOBOX)) return &combobox_ops;
    return nullptr;
  }

  static std::wstring ToWide(const char* utf8)
  {
    if (!utf8 || !*utf8) return {};
    int len = MultiByteToWideChar(CP_UTF8, 0, utf8, -1, nullptr, 0);
    if (len <= 0) return {};
    std::wstring result(len - 1, L'\0');
    MultiByteToWideChar(CP_UTF8, 0, utf8, -1, &result[0], len);
    return result;
  }

  static std::string FromWide(const wchar_t* wide, int wlen)
  {
    if (!wide || wlen == 0) return {};
    int len = WideCharToMultiByte(CP_UTF8, 0, wide, wlen, nullptr, 0, nullptr, nullptr);
    if (len <= 0) return {};
    std::string result(len, '\0');
    WideCharToMultiByte(CP_UTF8, 0, wide, wlen, &result[0], len, nullptr, nullptr);
    return result;
  }

  static void ApplyText(HWND hwnd, const std::string& text)
  {
    if (hwnd && !text.empty()) {
      SetWindowTextW(hwnd, ToWide(text.c_str()).c_str());
    }
  }

  static HFONT CreateDpiFont(UINT dpi)
  {
    NONCLIENTMETRICSW ncm = {};
    ncm.cbSize = sizeof(ncm);
    SystemParametersInfoForDpi(SPI_GETNONCLIENTMETRICS, sizeof(ncm), &ncm, 0, dpi);
    return CreateFontIndirectW(&ncm.lfMessageFont);
  }

  // -------------------------------------------------------------------------
  // Image asset helpers - thin wrapper over the shared WIC decoder in
  // hosts/shared/win32/image_loader_win32.h (resource-first, then disk).

  static uint8_t* load_wic_pixels(const char* path, uint32_t* w_out, uint32_t* h_out)
  {
    return neui_detail::load_image_bgra8_w32(path, w_out, h_out);
  }

  // Resolve @2x / @3x filename variants. Returns the best available path, or "".
  // Preference order matches DPI scale; if no preferred variant nor the base
  // file exists, the higher-resolution variants are tried as a last-resort
  // fallback so a deployment that ships only @2x assets still loads on a
  // 96-DPI screen (the bitmap is then downscaled at draw time).
  static std::string resolve_image_path(const std::string& name, float scale)
  {
    auto dot = name.rfind('.');
    std::string base = (dot == std::string::npos) ? name : name.substr(0, dot);
    std::string ext  = (dot == std::string::npos) ? "" : name.substr(dot);

    auto try_load = [&](const std::string& path) -> bool {
      uint32_t w = 0, h = 0;
      uint8_t* raw = load_wic_pixels(path.c_str(), &w, &h);
      if (!raw) return false;
      delete[] raw;
      return true;
    };

    if (scale > 2.0f) {
      if (try_load(base + "@3x" + ext)) return base + "@3x" + ext;
      if (try_load(base + "@2x" + ext)) return base + "@2x" + ext;
    } else if (scale > 1.0f) {
      if (try_load(base + "@2x" + ext)) return base + "@2x" + ext;
    }
    if (try_load(name)) return name;
    // Final fallback: try the higher-res variants the scale branch above
    // didn't already attempt. Lets a deployment ship only @2x (or @3x)
    // assets without a base file - on a 96-DPI screen the bitmap then
    // displays at its native pixel density, downscaled at draw time.
    if (scale <= 1.0f) {
      if (try_load(base + "@2x" + ext)) return base + "@2x" + ext;
    }
    if (scale <= 2.0f) {
      if (try_load(base + "@3x" + ext)) return base + "@3x" + ext;
    }
    return {};
  }

  // Load (or reload) the D2D image bitmap for an image widget.
  // Requires wd.image_ctx to already be set (done in ImageWndProc WM_CREATE).
  static void load_image_bitmap(WidgetData& wd, UINT parent_dpi)
  {
    auto* backend = neui_d2d_backend::get_backend();
    if (!backend || !wd.image_ctx) return;

    // Release old D2D bitmap.
    if (wd.image_bitmap) {
      backend->destroy_bitmap(wd.image_ctx, wd.image_bitmap);
      wd.image_bitmap     = nullptr;
      wd.image_scale      = 0.0f;
      wd.image_width_px   = 0;
      wd.image_height_px  = 0;
    }
    if (wd.text.empty()) return;

    float scale = static_cast<float>(parent_dpi) / 96.0f;
    std::string path = resolve_image_path(wd.text, scale);
    if (path.empty()) return;

    // Determine actual scale of the resolved path.
    auto dot  = wd.text.rfind('.');
    std::string base = (dot == std::string::npos) ? wd.text : wd.text.substr(0, dot);
    std::string ext  = (dot == std::string::npos) ? "" : wd.text.substr(dot);
    float actual_scale = (path == base + "@3x" + ext) ? 3.0f
                       : (path == base + "@2x" + ext) ? 2.0f
                       : 1.0f;

    uint32_t w = 0, h = 0;
    uint8_t* pixels = load_wic_pixels(path.c_str(), &w, &h);
    if (!pixels) return;

    wd.image_bitmap     = backend->create_bitmap(wd.image_ctx, w, h, pixels, actual_scale);
    wd.image_width_px   = w;
    wd.image_height_px  = h;
    delete[] pixels;
    wd.image_scale = actual_scale;

    if (wd.hwnd) InvalidateRect(wd.hwnd, nullptr, TRUE);
  }

  // -------------------------------------------------------------------------
  // Self-painted widget hooks (for the shared "neui.painted" window class).

  static float clamp01_w32(float v)
  {
    if (v < 0.0f) return 0.0f;
    if (v > 1.0f) return 1.0f;
    return v;
  }

  // Snap v to one of N evenly-spaced positions on [0..1]. steps < 2 → no snap.
  static float snap_to_steps_w32(float v, int steps)
  {
    if (steps < 2) return v;
    int idx = static_cast<int>(v * static_cast<float>(steps - 1) + 0.5f);
    if (idx < 0) idx = 0;
    if (idx >= steps) idx = steps - 1;
    return static_cast<float>(idx) / static_cast<float>(steps - 1);
  }

  static int widget_get_steps_w32(const WidgetData& wd)
  {
    return wd.attrs ? wd.attrs->get_int(NEUI_ATTR_STEPS, 0) : 0;
  }

  // Discrete-or-continuous nudge delta; mirrors xpl host's nudge_delta.
  static float nudge_delta_w32(const WidgetData& wd,
                                int   step_count,
                                float continuous_amount)
  {
    int steps = widget_get_steps_w32(wd);
    if (steps >= 2) {
      return static_cast<float>(step_count) / static_cast<float>(steps - 1);
    }
    return continuous_amount;
  }

  static float widget_get_value_w32(const WidgetData& wd)
  {
    if (!wd.attrs) return 0.0f;
    return clamp01_w32(wd.attrs->get_float(NEUI_PARAM_VALUE, 0.0f));
  }

  // Write value silently (no event); used by drag/wheel after mutating.
  // Fires VALUE_CHANGED iff the value actually changed and emit_events is set.
  static void widget_set_value_user_w32(WidgetData& wd, float v)
  {
    v = snap_to_steps_w32(clamp01_w32(v), widget_get_steps_w32(wd));
    float old = widget_get_value_w32(wd);
    if (v == old) return;
    neui_detail::ensure_attrs(wd.attrs).set_float(NEUI_PARAM_VALUE, v);
    if (wd.emit_events && wd.session) {
      neui_widget_t wid = { wd.widget_id };
      neui_event_t ev{};
      ev.type = NEUI_EVENT_VALUE_CHANGED;
      ev.data.value.widget = wid;
      ev.data.value.value  = v;
      wd.session->dispatch_event(&ev);
    }
    // Push to the native control if applicable. Slider needs TBM_SETPOS to
    // move the thumb; painted widgets just need an InvalidateRect.
    if (wd.hwnd && wd.type && !strcmp(wd.type, NEUI_W_SLIDER)) {
      DWORD style    = static_cast<DWORD>(GetWindowLongW(wd.hwnd, GWL_STYLE));
      bool  vertical = (style & TBS_VERT) != 0;
      int   pos      = static_cast<int>((vertical ? (1.0f - v) : v) * 1000.0f + 0.5f);
      SendMessageW(wd.hwnd, TBM_SETPOS, TRUE, pos);
    } else if (wd.hwnd) {
      InvalidateRect(wd.hwnd, nullptr, FALSE);
    }
  }

  // Reset a value-bearing widget to NEUI_PARAM_DEFAULT (or 0 if unset).
  // Non-static so window.cpp's ChildSubclassProc can call it for slider
  // double-click. Public within the win32_host namespace.
  void widget_reset_to_default_w32(WidgetData& wd)
  {
    float def = wd.attrs ? wd.attrs->get_float(NEUI_PARAM_DEFAULT, 0.0f) : 0.0f;
    widget_set_value_user_w32(wd, def);
  }

  // Paint hook used by the "neui.painted" class for KNOB widgets.
  // The shared PaintedWndProc has already cleared the frame to the parent's
  // system background colour, so the bounding rect outside the disc shows
  // that colour - making the knob visually "transparent" to its parent.
  static void paint_knob_w32(neui_render_backend_t* backend,
                              neui_render_ctx_t      ctx,
                              float w, float h,
                              WidgetData&            wd,
                              bool                   focused)
  {
    auto polarity = neui_detail::KNOB_POLARITY_MIN;
    const char* value_text = nullptr;
    if (wd.attrs) {
      polarity   = neui_detail::parse_knob_polarity(wd.attrs->get_string(NEUI_ATTR_POLARITY));
      value_text = wd.attrs->get_string(NEUI_ATTR_VALUE_TEXT);
    }
    int steps = widget_get_steps_w32(wd);
    neui_detail::paint_knob(backend, ctx, 0.0f, 0.0f, w, h,
                             widget_get_value_w32(wd), focused,
                             polarity, steps, value_text);
  }

  // Mouse / wheel hook for KNOB. Implements angular-drag-to-value: the
  // value tracks the cursor's rotation around the knob centre. Per-pixel
  // sensitivity is the natural 1/radius (small circles move the value
  // faster than wide ones). Shift = 1/5 sensitivity, wheel for nudges,
  // double-click = reset to NEUI_PARAM_DEFAULT.
  static constexpr float KNOB_W32_SWEEP_RAD     = 4.71238898f; // 1.5 * PI (270°)
  static constexpr float KNOB_W32_DEAD_ZONE_R   = 4.0f;        // logical px
  static constexpr float KNOB_W32_FINE_SCALE    = 0.2f;        // Shift = 1/5 sensitivity
  // Slider modes (vertical / horizontal NEUI_ATTR_KNOB_MODE): logical-pixel
  // sweep distance for a full [0..1] traversal.
  static constexpr float KNOB_W32_SLIDER_SWEEP_PX = 200.0f;

  static float wrap_pi_w32(float d)
  {
    const float TWO_PI = 6.28318530717958647692f;
    while (d >  3.14159265358979323846f) d -= TWO_PI;
    while (d < -3.14159265358979323846f) d += TWO_PI;
    return d;
  }

  // Show the knob's right-click context menu and act on the pick.
  // Local coords are in HWND-local logical pixels; popup_menu maps to
  // screen for us.
  static void knob_show_context_menu_w32(WidgetData& wd, int local_x, int local_y)
  {
    if (!wd.session) return;
    static const char* k_items[] = {
      "Reset to default",
      nullptr
    };
    neui_widget_t anchor = { wd.widget_id };
    int picked = popup_menu({ wd.session_id }, anchor, local_x, local_y, k_items);
    if (picked == 1) {
      widget_reset_to_default_w32(wd);
    }
  }

  static void painted_msg_knob_w32(WidgetData& wd, UINT msg,
                                    WPARAM wParam, LPARAM lParam)
  {
    if (msg == WM_MOUSEWHEEL) {
      short delta = static_cast<short>(HIWORD(wParam));
      bool fine = (LOWORD(wParam) & MK_SHIFT) != 0;
      // Wheel up DECREASES knob value, wheel down INCREASES - matches
      // audio-plugin convention.
      float step = nudge_delta_w32(wd, 1, fine ? 0.01f : 0.05f) *
                   (delta > 0 ? -1.0f : 1.0f);
      widget_set_value_user_w32(wd, widget_get_value_w32(wd) + step);
      return;
    }

    if (msg == WM_KEYDOWN) {
      // Arrows / Home / End. Snap-aware via nudge_delta_w32. Continuous
      // mode advances by 10% per arrow press (10 presses end-to-end);
      // stepped mode advances by exactly one step. (`small` is a Win32
      // macro for `char`, hence the renamed local.)
      float v    = widget_get_value_w32(wd);
      float step = nudge_delta_w32(wd, 1, 0.10f);
      bool handled = true;
      switch (wParam) {
        case VK_LEFT:
        case VK_DOWN:  v -= step; break;
        case VK_RIGHT:
        case VK_UP:    v += step; break;
        case VK_HOME:  v = 0.0f; break;
        case VK_END:   v = 1.0f; break;
        default:       handled = false; break;
      }
      if (handled)
        widget_set_value_user_w32(wd, v);
      return;
    }

    UINT dpi = (wd.session ? wd.session->get_dpi_for_widget(wd.index) : 96);
    if (dpi == 0) dpi = 96;

    if (msg == WM_LBUTTONDBLCLK) {
      float def = wd.attrs ? clamp01_w32(wd.attrs->get_float(NEUI_PARAM_DEFAULT, 0.0f))
                            : 0.0f;
      widget_set_value_user_w32(wd, def);
      return;
    }

    if (msg == WM_RBUTTONDOWN) {
      int lx = MulDiv(GET_X_LPARAM(lParam), 96, static_cast<int>(dpi));
      int ly = MulDiv(GET_Y_LPARAM(lParam), 96, static_cast<int>(dpi));
      knob_show_context_menu_w32(wd, lx, ly);
      return;
    }

    // For DOWN / UP / MOVE we work in HWND-local logical pixels. The knob
    // centre is the centre of the client rect.
    RECT rc{};
    if (wd.hwnd) GetClientRect(wd.hwnd, &rc);
    float cx = static_cast<float>(MulDiv(rc.right,  96, static_cast<int>(dpi))) * 0.5f;
    float cy = static_cast<float>(MulDiv(rc.bottom, 96, static_cast<int>(dpi))) * 0.5f;
    float mx = static_cast<float>(MulDiv(GET_X_LPARAM(lParam), 96, static_cast<int>(dpi)));
    float my = static_cast<float>(MulDiv(GET_Y_LPARAM(lParam), 96, static_cast<int>(dpi)));

    if (msg == WM_LBUTTONDOWN) {
      // Cache the drag mode for the duration of this drag so WM_MOUSEMOVE
      // doesn't pay the attribute-lookup cost. Live changes to the attr
      // take effect on the NEXT drag.
      wd.paint_drag_mode = wd.attrs
        ? wd.attrs->get_int(NEUI_ATTR_KNOB_MODE, NEUI_KNOB_MODE_ROTATIONAL)
        : NEUI_KNOB_MODE_ROTATIONAL;
      int imx = static_cast<int>(mx);
      int imy = static_cast<int>(my);
      if (wd.paint_drag_mode == NEUI_KNOB_MODE_ROTATIONAL) {
        float dx = mx - cx;
        float dy = my - cy;
        if (dx*dx + dy*dy < KNOB_W32_DEAD_ZONE_R * KNOB_W32_DEAD_ZONE_R) {
          // Click bang in the centre: don't start a drag we can't track.
          return;
        }
        wd.paint_drag_prev_angle = std::atan2(dy, dx);
      } else {
        wd.paint_drag_prev_x = imx;
        wd.paint_drag_prev_y = imy;
      }
      wd.paint_dragging = true;
      // Seed the continuous accumulator with the current snapped value so
      // small per-frame deltas accumulate into step crossings rather than
      // being rounded back to the same snapped value each sample.
      wd.paint_drag_continuous = widget_get_value_w32(wd);
      SetFocus(wd.hwnd);
      return;
    }
    if (msg == WM_LBUTTONUP) {
      wd.paint_dragging = false;
      return;
    }
    if (msg == WM_MOUSEMOVE && wd.paint_dragging) {
      bool  fine     = (wParam & MK_SHIFT) != 0;
      float fine_mul = fine ? KNOB_W32_FINE_SCALE : 1.0f;
      float delta_v  = 0.0f;
      int   imx      = static_cast<int>(mx);
      int   imy      = static_cast<int>(my);
      if (wd.paint_drag_mode == NEUI_KNOB_MODE_VERTICAL) {
        // Up = increase: negative pixel delta -> positive value delta.
        delta_v = -static_cast<float>(imy - wd.paint_drag_prev_y) *
                  (fine_mul / KNOB_W32_SLIDER_SWEEP_PX);
        wd.paint_drag_prev_y = imy;
      } else if (wd.paint_drag_mode == NEUI_KNOB_MODE_HORIZONTAL) {
        delta_v = static_cast<float>(imx - wd.paint_drag_prev_x) *
                  (fine_mul / KNOB_W32_SLIDER_SWEEP_PX);
        wd.paint_drag_prev_x = imx;
      } else {
        float dx = mx - cx;
        float dy = my - cy;
        float r2 = dx*dx + dy*dy;
        if (r2 < KNOB_W32_DEAD_ZONE_R * KNOB_W32_DEAD_ZONE_R) {
          // Inside dead zone - angle is unstable. Drop this sample, but
          // don't end the drag; the user may slip back out.
          return;
        }
        float cur_angle = std::atan2(dy, dx);
        delta_v = wrap_pi_w32(cur_angle - wd.paint_drag_prev_angle) *
                  (fine_mul / KNOB_W32_SWEEP_RAD);
        wd.paint_drag_prev_angle = cur_angle;
      }
      // Accumulate continuously so small per-frame deltas survive across
      // step snapping; only the snapped value is published externally.
      wd.paint_drag_continuous += delta_v;
      wd.paint_drag_continuous = clamp01_w32(wd.paint_drag_continuous);
      widget_set_value_user_w32(wd, wd.paint_drag_continuous);
      return;
    }
  }

  // -------------------------------------------------------------------------
  // SECTION (neui.painted seam) - non-interactive coloured backdrop with
  // an optional title chip. paint_section_w32 delegates to the shared
  // helper so the visual matches the xpl host. apply_section_region_w32
  // installs a window region (body rect + chip rect, in physical px) so
  // the title band's non-chip area is truly transparent and the parent
  // frame's pixels show through underneath.

  static void paint_section_w32(neui_render_backend_t* backend,
                                 neui_render_ctx_t      ctx,
                                 float w, float h,
                                 WidgetData&            wd,
                                 bool                   /*focused*/)
  {
    using neui_detail::ColorRole;
    uint32_t bg = neui_detail::shade(
                    neui_detail::color(ColorRole::frame_bg),
                    neui_detail::SECTION_BG_LIFT);
    if (wd.attrs && wd.attrs->has(NEUI_ATTR_BACKGROUND))
      bg = static_cast<uint32_t>(wd.attrs->get_int(NEUI_ATTR_BACKGROUND, 0));
    const char* align = wd.attrs ? wd.attrs->get_string(NEUI_ATTR_ALIGN_TEXT) : nullptr;
    neui_detail::paint_section(backend, ctx, 0.0f, 0.0f, w, h,
                                wd.text.c_str(), bg, align,
                                neui_detail::color(ColorRole::text_primary));
  }

  // Build and apply the section's window region so anything outside
  // (body ∪ title chip) is clipped away. Called from PaintedWndProc on
  // create + size, and from set_text / NEUI_ATTR_ALIGN_TEXT live updates.
  // Logical (96 DPI) measurements are converted to physical px for the
  // GDI region. If `text` is empty the region is the full rect (no band).
  // Non-static so PaintedWndProc in window.cpp can call it from WM_SIZE.
  void apply_section_region_w32(WidgetData& wd)
  {
    if (!wd.hwnd) return;
    UINT dpi = wd.session ? wd.session->get_dpi_for_widget(wd.index) : 96;
    if (dpi == 0) dpi = 96;
    int phys_w = LogicalToPhysical(wd.width,  dpi);
    int phys_h = LogicalToPhysical(wd.height, dpi);
    if (phys_w <= 0 || phys_h <= 0) {
      SetWindowRgn(wd.hwnd, nullptr, FALSE);
      return;
    }

    if (wd.text.empty()) {
      // No band reserved - full rect.
      SetWindowRgn(wd.hwnd, CreateRectRgn(0, 0, phys_w, phys_h), FALSE);
      return;
    }

    auto* backend = neui_d2d_backend::get_backend();
    float tw = (backend && backend->measure_text)
                 ? backend->measure_text(nullptr, wd.text.c_str(), -1,
                                          neui_detail::SECTION_LABEL_FONT)
                 : 0.0f;
    const char* align = wd.attrs ? wd.attrs->get_string(NEUI_ATTR_ALIGN_TEXT) : nullptr;
    auto chip = neui_detail::section_chip_rect(
                  0.0f,
                  static_cast<float>(wd.width),
                  static_cast<float>(wd.height),
                  tw, align);

    // Round to nearest physical pixel; chip width is clamped to widget
    // width by the helper so chip_right cannot overflow phys_w.
    auto to_phys = [dpi](float logical) {
      return LogicalToPhysical(static_cast<int>(logical + 0.5f), dpi);
    };
    int band_h_phys = to_phys(chip.band_h);
    int chip_l = to_phys(chip.chip_x);
    int chip_r = to_phys(chip.chip_x + chip.chip_w);
    if (band_h_phys > phys_h) band_h_phys = phys_h;
    if (chip_l < 0)           chip_l      = 0;
    if (chip_r > phys_w)      chip_r      = phys_w;

    HRGN body = CreateRectRgn(0, band_h_phys, phys_w, phys_h);
    HRGN chip_rgn = CreateRectRgn(chip_l, 0, chip_r, band_h_phys);
    CombineRgn(body, body, chip_rgn, RGN_OR);
    DeleteObject(chip_rgn);
    // SetWindowRgn takes ownership of `body`; do not delete it ourselves.
    // bRedraw=FALSE so we don't loop when called from inside WM_PAINT.
    SetWindowRgn(wd.hwnd, body, FALSE);

    // The region may have SHRUNK relative to its previous shape (e.g.
    // the chip just moved from "left" to "center", so the strip the chip
    // used to occupy at the left of the band is now outside the region).
    // Windows does not auto-repaint the parent in pixels a child window
    // just stopped covering, so the old chip's frame_bg "ghost" lingers
    // there. Invalidate the parent in the section's full bounding rect;
    // WS_CLIPCHILDREN on the frame clips the parent's paint to the area
    // OUTSIDE the section's (new) region, so only the newly-exposed
    // pixels actually repaint.
    if (HWND parent = GetParent(wd.hwnd)) {
      RECT sec_rect;
      GetWindowRect(wd.hwnd, &sec_rect);
      MapWindowPoints(HWND_DESKTOP, parent,
                      reinterpret_cast<POINT*>(&sec_rect), 2);
      InvalidateRect(parent, &sec_rect, TRUE);
    }
  }

  // -------------------------------------------------------------------------

  static HWND CreateChildHwnd(WidgetData& wd, HWND parent_hwnd, UINT parent_dpi)
  {
    // Image widget uses a custom WndProc (not a standard control class).
    if (!strcmp(wd.type, NEUI_W_IMAGE)) {
      DWORD style = WS_CHILD;
      if (wd.visible) style |= WS_VISIBLE;
      return CreateWindowExW(0, L"neui.image", L"", style,
        LogicalToPhysical(wd.x, parent_dpi),
        LogicalToPhysical(wd.y, parent_dpi),
        LogicalToPhysical(wd.width, parent_dpi),
        LogicalToPhysical(wd.height, parent_dpi),
        parent_hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(wd.index)),
        get_hinstance(),
        &wd);
    }

    // Knob (and future painted controls) use the shared "neui.painted" class.
    // Paint logic and mouse hooks plug in via WidgetData function pointers.
    if (!strcmp(wd.type, NEUI_W_KNOB)) {
      DWORD style = WS_CHILD | WS_TABSTOP;
      if (wd.visible) style |= WS_VISIBLE;
      wd.paint_fn       = &paint_knob_w32;
      wd.painted_msg_fn = &painted_msg_knob_w32;
      HWND hwnd = CreateWindowExW(0, L"neui.painted", L"", style,
        LogicalToPhysical(wd.x, parent_dpi),
        LogicalToPhysical(wd.y, parent_dpi),
        LogicalToPhysical(wd.width, parent_dpi),
        LogicalToPhysical(wd.height, parent_dpi),
        parent_hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(wd.index)),
        get_hinstance(),
        &wd);
      if (hwnd) {
        SetWindowSubclass(hwnd, ChildSubclassProc, 1, reinterpret_cast<DWORD_PTR>(&wd));
        wd.has_subclass = true;
      }
      return hwnd;
    }

    // Section: non-interactive painted container. No tab stop, no mouse
    // hook. WS_CLIPSIBLINGS so the section's paint stays clipped out of
    // its child siblings' rects (children parent to the frame and sit on
    // top of the section in z-order). After the HWND exists, the window
    // region is installed so the band's non-chip area is transparent.
    if (!strcmp(wd.type, NEUI_W_SECTION)) {
      DWORD style = WS_CHILD | WS_CLIPSIBLINGS;
      if (wd.visible) style |= WS_VISIBLE;
      wd.paint_fn = &paint_section_w32;
      HWND hwnd = CreateWindowExW(0, L"neui.painted", L"", style,
        LogicalToPhysical(wd.x, parent_dpi),
        LogicalToPhysical(wd.y, parent_dpi),
        LogicalToPhysical(wd.width, parent_dpi),
        LogicalToPhysical(wd.height, parent_dpi),
        parent_hwnd,
        reinterpret_cast<HMENU>(static_cast<UINT_PTR>(wd.index)),
        get_hinstance(),
        &wd);
      if (hwnd) {
        wd.hwnd = hwnd;
        apply_section_region_w32(wd);
      }
      return hwnd;
    }

    LPCWSTR wclass = nullptr;
    DWORD style = WS_CHILD;
    bool needs_subclass = false;

    for (auto& t : widget_type_table) {
      if (!strcmp(wd.type, t.neui_type)) {
        wclass = t.win32_class;
        style = t.style;
        needs_subclass = t.needs_subclass;
        break;
      }
    }

    if (!wclass) return nullptr;

    // Attribute-driven style overrides. The attribute bag is the authoritative
    // source for orthogonal features; widget-type strings only set defaults.
    if (wd.attrs) {
      if (!strcmp(wd.type, NEUI_W_CHECKBOX) &&
          wd.attrs->get_int(NEUI_ATTR_TRISTATE, 0))
      {
        style = (style & ~static_cast<DWORD>(BS_AUTOCHECKBOX)) | BS_AUTO3STATE;
      }
      if (!strcmp(wd.type, NEUI_W_SLIDER)) {
        const char* o = wd.attrs->get_string(NEUI_ATTR_ORIENTATION);
        if (o && !strcmp(o, "vertical"))
          style = (style & ~static_cast<DWORD>(TBS_HORZ)) | TBS_VERT;
        // Tick marks: NEUI_ATTR_STEPS >= 2 → clear TBS_NOTICKS so user-
        // placed ticks render. We do NOT use TBS_AUTOTICKS because its
        // frequency (TBM_SETTICFREQ) must be integer; for non-divisible
        // step counts (e.g. 16 over a 1000 range) it rounds down and the
        // error compounds - visually the ticks drift left toward higher
        // values. We place each tick manually via TBM_SETTIC instead.
        if (wd.attrs->get_int(NEUI_ATTR_STEPS, 0) >= 2)
          style = (style & ~static_cast<DWORD>(TBS_NOTICKS));
      }
    }

    if (wd.visible) style |= WS_VISIBLE;

    HWND hwnd = CreateWindowExW(
      0,
      wclass,
      L"",
      style,
      LogicalToPhysical(wd.x, parent_dpi),
      LogicalToPhysical(wd.y, parent_dpi),
      LogicalToPhysical(wd.width, parent_dpi),
      LogicalToPhysical(wd.height, parent_dpi),
      parent_hwnd,
      reinterpret_cast<HMENU>(static_cast<UINT_PTR>(wd.index)),
      get_hinstance(),
      nullptr
    );

    if (hwnd) {
      ApplyText(hwnd, wd.text);
      if (needs_subclass) {
        SetWindowSubclass(hwnd, ChildSubclassProc, 1, reinterpret_cast<DWORD_PTR>(&wd));
        wd.has_subclass = true;
      }
      // Stash the HWND on the WidgetData before applying the visual
      // theme (apply_native_theme_w32 reads wd.hwnd to drive
      // SetWindowTheme + treeview colours).
      wd.hwnd = hwnd;
      apply_native_theme_w32(wd);
      // Slider: configure the trackbar's range and initial position from
      // NEUI_PARAM_VALUE. The internal range is 0..1000, exposed externally
      // as a normalized [0..1] float via NEUI_PARAM_VALUE.
      if (!strcmp(wd.type, NEUI_W_SLIDER)) {
        SendMessageW(hwnd, TBM_SETRANGE, FALSE, MAKELPARAM(0, 1000));
        int steps = wd.attrs ? wd.attrs->get_int(NEUI_ATTR_STEPS, 0) : 0;
        if (steps >= 2) {
          // Place each interior tick at the precisely-rounded position.
          // The endpoints are drawn automatically by the trackbar; we
          // only need the (steps - 2) interior ticks. Computing each
          // one as round(i / (steps-1) * 1000) avoids the cumulative
          // drift that TBM_SETTICFREQ produces with non-divisible counts.
          SendMessageW(hwnd, TBM_CLEARTICS, FALSE, 0);
          for (int i = 1; i < steps - 1; ++i) {
            int pos = static_cast<int>(
              static_cast<float>(i) /
              static_cast<float>(steps - 1) * 1000.0f + 0.5f);
            SendMessageW(hwnd, TBM_SETTIC, 0, pos);
          }
          // Arrow keys advance one step; Page keys advance two. The
          // WM_HSCROLL / WM_VSCROLL handler snaps the resulting position
          // to the exact step, so any rounding here is invisible.
          int line_size = 1000 / (steps - 1);
          if (line_size < 1) line_size = 1;
          SendMessageW(hwnd, TBM_SETLINESIZE, 0, line_size);
          SendMessageW(hwnd, TBM_SETPAGESIZE, 0, line_size * 2);
        } else {
          // Continuous mode: arrow keys move by 10% of the range, Page
          // keys by 20%. The trackbar's defaults (1 and 20) would mean
          // 100 arrow presses to traverse the slider, which is unusable.
          SendMessageW(hwnd, TBM_SETLINESIZE, 0, 100);
          SendMessageW(hwnd, TBM_SETPAGESIZE, 0, 200);
        }
        float v = clamp01_w32(wd.attrs ? wd.attrs->get_float(NEUI_PARAM_VALUE, 0.0f) : 0.0f);
        v = snap_to_steps_w32(v, steps);
        // Vertical: 1.0 = top in our convention, but Win32 default = 0 at top.
        bool vertical = (style & TBS_VERT) != 0;
        int  pos = static_cast<int>((vertical ? (1.0f - v) : v) * 1000.0f + 0.5f);
        SendMessageW(hwnd, TBM_SETPOS, TRUE, pos);
      }
    }

    return hwnd;
  }

  // Session member functions

  bool Session::run()
  {
    return win32_host::run();
  }

  WidgetData* Session::get_widget(uint32_t index)
  {
    if (_widgets.exists(index)) {
      return &_widgets[index];
    }
    return nullptr;
  }

  uint32_t Session::get_dpi_for_widget(uint32_t index)
  {
    if (!_widgets.exists(index)) return 96;
    auto& wd = _widgets[index];
    // Frame windows have their own DPI; check this widget first
    if (wd.hwnd && (wd.isroot || !strcmp(wd.type, NEUI_W_PLUGWINDOW))) {
      return wd.dpi;
    }
    // Walk parent chain to find the nearest frame window
    auto parents = _widgets.get_all_parents(index);
    for (auto p : parents) {
      if (p == 0) continue;
      if (_widgets.exists(p)) {
        auto& pd = _widgets[p];
        if (pd.hwnd && (pd.isroot || !strcmp(pd.type, NEUI_W_PLUGWINDOW))) {
          return pd.dpi;
        }
      }
    }
    return 96;
  }

  void Session::cascade_dpi(uint32_t parent_index, UINT new_dpi)
  {
    if (new_dpi == 0) new_dpi = 96;
    uint32_t child_idx = _widgets.child(parent_index);
    while (child_idx != 0) {
      if (_widgets.exists(child_idx)) {
        auto& wd = _widgets[child_idx];
        if (wd.hwnd) {
          wd.dpi = new_dpi;
          // Drop the cached DPI font on every container that has one
          // (typically painted widgets that act as parents - SECTION
          // most importantly). The follow-up create_child_windows will
          // recreate it at the new DPI; without this the stale font
          // would be re-broadcast to grandchildren via WM_SETFONT.
          if (wd.hfont) {
            DeleteObject(wd.hfont);
            wd.hfont = nullptr;
          }
          // Reposition + resize this child to its logical geometry at the
          // new DPI. wd.x / wd.y are parent-relative logical pixels; the
          // new physical px land at the right spot in the parent HWND's
          // (already-resized) client area.
          int new_x = LogicalToPhysical(wd.x, new_dpi);
          int new_y = LogicalToPhysical(wd.y, new_dpi);
          int new_w = LogicalToPhysical(wd.width,  new_dpi);
          int new_h = LogicalToPhysical(wd.height, new_dpi);
          SetWindowPos(wd.hwnd, nullptr, new_x, new_y, new_w, new_h,
                       SWP_NOZORDER | SWP_NOACTIVATE);
          // Painted widgets (KNOB, SECTION, IMAGE) own a per-widget D2D
          // context whose DPI must be reset so DrawText / fill_rect map
          // logical → physical at the new ratio. The SetWindowPos above
          // already fires WM_SIZE which handles the swap-chain resize.
          auto* backend = neui_d2d_backend::get_backend();
          if (backend && backend->update_dpi) {
            if (wd.paint_ctx) backend->update_dpi(wd.paint_ctx, new_dpi);
            if (wd.image_ctx) backend->update_dpi(wd.image_ctx, new_dpi);
          }
          // Section: window region was computed in old physical px;
          // recompute against the new DPI.
          if (wd.type && !strcmp(wd.type, NEUI_W_SECTION))
            apply_section_region_w32(wd);
          // Image: reload the bitmap at the new logical→physical scale
          // (matches the WM_DPICHANGED branch in create_child_windows).
          if (wd.type && !strcmp(wd.type, NEUI_W_IMAGE) && wd.image_ctx) {
            float new_scale = static_cast<float>(new_dpi) / 96.0f;
            if (new_scale != wd.image_scale)
              load_image_bitmap(wd, new_dpi);
          }
          // Native control refresh belt-and-suspenders: most native
          // controls auto-recompute row layout when WM_SETFONT arrives
          // from the follow-up create_child_windows pass, but a handful
          // need explicit messages or cache stale values across DPI
          // flips. These are cheap; no-op when the message is ignored.
          if (wd.type) {
            // TreeView: force item-height recompute from font. Without
            // this the rows can keep the old DPI's metrics until the
            // first item is added/removed.
            if (!strcmp(wd.type, NEUI_W_TREEVIEW))
              SendMessageW(wd.hwnd, TVM_SETITEMHEIGHT,
                           static_cast<WPARAM>(-1), 0);
            // ListBox / ComboBox: invalidate so the new font is picked
            // up across already-laid-out items. SetWindowPos above
            // invalidates on size change, but a same-size DPI flip
            // (rare, but possible across identical-DPI monitors with
            // different scaling preferences) wouldn't trigger it.
            if (!strcmp(wd.type, NEUI_W_LISTBOX)  ||
                !strcmp(wd.type, NEUI_W_COMBOBOX) ||
                !strcmp(wd.type, NEUI_W_MULTILINE))
              InvalidateRect(wd.hwnd, nullptr, TRUE);
          }
        }
        cascade_dpi(child_idx, new_dpi);
      }
      child_idx = _widgets.next(child_idx);
    }
  }

  void Session::create_child_windows(uint32_t parent_index)
  {
    if (!_widgets.exists(parent_index)) return;
    auto& parent_wd = _widgets[parent_index];
    HWND parent_hwnd = parent_wd.hwnd;
    UINT parent_dpi  = parent_wd.dpi;

    // Create a DPI-scaled font for this frame window on first use
    if (parent_wd.hfont == nullptr && parent_dpi > 0) {
      parent_wd.hfont = CreateDpiFont(parent_dpi);
    }

    uint32_t child_idx = _widgets.child(parent_index);
    while (child_idx != 0) {
      if (_widgets.exists(child_idx)) {
        auto& child = _widgets[child_idx];
        if (child.hmenu != nullptr && child.hwnd == nullptr) {
          // MENUBAR: attach to the parent frame window (HMENU was created on widget_create)
          SetMenu(parent_hwnd, child.hmenu);
          DrawMenuBar(parent_hwnd);
        } else if (child.hwnd == nullptr) {
          child.hwnd = CreateChildHwnd(child, parent_hwnd, parent_dpi);
          // Flush items buffered before the HWND existed (listbox/combobox)
          if (child.hwnd && !child.pending_items.empty()) {
            auto* ops = get_items_ops(child.type);
            if (ops) {
              for (auto& item : child.pending_items) {
                auto wtext = ToWide(item.text.c_str());
                LRESULT idx = SendMessageW(child.hwnd, ops->add, 0, (LPARAM)wtext.c_str());
                if (idx >= 0)
                  SendMessageW(child.hwnd, ops->setdata, (WPARAM)idx, (LPARAM)item.userdata);
              }
              child.pending_items.clear();
              if (child.pending_selection != NEUI_ITEM_NONE) {
                SendMessageW(child.hwnd, ops->setsel, (WPARAM)child.pending_selection, 0);
                child.pending_selection = NEUI_ITEM_NONE;
              }
            }
          }
          // Flush pending treeview items
          if (child.hwnd && !child.pending_tree_items.empty()) {
            for (auto& pitem : child.pending_tree_items) {
              auto it = child.tree_items.find(pitem.neui_id);
              if (it == child.tree_items.end()) continue;
              auto& data = it->second;

              HTREEITEM parent_hitem = TVI_ROOT;
              if (pitem.parent_neui_id != 0) {
                auto pit = child.tree_items.find(pitem.parent_neui_id);
                if (pit != child.tree_items.end())
                  parent_hitem = pit->second.hitem;
              }

              TVINSERTSTRUCTW tvis = {};
              tvis.hParent      = parent_hitem;
              tvis.hInsertAfter = TVI_LAST;
              tvis.item.mask    = TVIF_TEXT | TVIF_PARAM;
              auto wtext        = ToWide(data.text.c_str());
              tvis.item.pszText = const_cast<LPWSTR>(wtext.c_str());
              tvis.item.lParam  = static_cast<LPARAM>(pitem.neui_id);
              HTREEITEM hitem   = (HTREEITEM)SendMessageW(child.hwnd, TVM_INSERTITEMW, 0, (LPARAM)&tvis);
              data.hitem = hitem;
              if (hitem)
                child.tree_items_reverse[reinterpret_cast<uintptr_t>(hitem)] = pitem.neui_id;
            }
            child.pending_tree_items.clear();
          }
          // Flush pending check state for checkboxes
          if (child.pending_check >= 0) {
            SendMessageW(child.hwnd, BM_SETCHECK, (WPARAM)child.pending_check, 0);
            child.pending_check = -1;
          }
        }
        if (child.hwnd && parent_wd.hfont) {
          SendMessageW(child.hwnd, WM_SETFONT, (WPARAM)parent_wd.hfont, TRUE);
        }
        // Reload image bitmap when DPI changes.
        if (child.hwnd && child.type && !strcmp(child.type, NEUI_W_IMAGE)) {
          float new_scale = static_cast<float>(parent_dpi) / 96.0f;
          if (new_scale != child.image_scale) {
            auto* backend = neui_d2d_backend::get_backend();
            if (backend && child.image_ctx)
              backend->update_dpi(child.image_ctx, parent_dpi);
            load_image_bitmap(child, parent_dpi);
          }
        }
        create_child_windows(child_idx);
      }
      child_idx = _widgets.next(child_idx);
    }
  }

  neui_widget_t Session::widget_create(neui_widget_t parent, const char* type, int x, int y, int width, int height, void* userdata)
  {
    auto widget_data = std::make_unique<WidgetData>();
    widget_data->type        = type;
    widget_data->x           = x;
    widget_data->y           = y;
    widget_data->width       = width;
    widget_data->height      = height;
    widget_data->visible     = true;
    // Auto-enable events for widget types whose documented behaviour
    // depends on event delivery (selection notifications, value changes,
    // etc.). Matches the xpl host and the auto-set list in CLAUDE.md so
    // both hosts behave the same out of the box.
    widget_data->emit_events = type && (!strcmp(type, NEUI_W_BUTTON)    ||
                                        !strcmp(type, NEUI_W_INPUTBOX)  ||
                                        !strcmp(type, NEUI_W_CHECKBOX)  ||
                                        !strcmp(type, NEUI_W_CHECKBOX3) ||
                                        !strcmp(type, NEUI_W_LISTBOX)   ||
                                        !strcmp(type, NEUI_W_COMBOBOX)  ||
                                        !strcmp(type, NEUI_W_MULTILINE) ||
                                        !strcmp(type, NEUI_W_TREEVIEW)  ||
                                        !strcmp(type, NEUI_W_SLIDER)    ||
                                        !strcmp(type, NEUI_W_KNOB));
    widget_data->userdata    = userdata;
    widget_data->session     = this;
    widget_data->session_id  = _session_id;

    // Map implicit type variants onto platform-neutral attributes so internal
    // behavior can be driven uniformly across hosts.
    if (type && !strcmp(type, NEUI_W_CHECKBOX3))
      neui_detail::ensure_attrs(widget_data->attrs).set_int(NEUI_ATTR_TRISTATE, 1);
    if (type && !strcmp(type, NEUI_W_MULTILINE))
      neui_detail::ensure_attrs(widget_data->attrs).set_int(NEUI_ATTR_MULTILINE, 1);

    neui_widget_t widget;

    if (parent.id == widget_none.id) {
      widget_data->isroot = true;
      uint32_t widget_id = _widgets.add_child(0, std::move(widget_data));
      _widgets[widget_id].index     = widget_id;
      _widgets[widget_id].widget_id = IndexToWidget(_session_id, widget_id).id;
      widget = IndexToWidget(_session_id, widget_id);
    } else {
      widget_data->isroot = false;
      uint32_t parent_index = WidgetToIndex(parent);
      uint32_t widget_id = _widgets.add_child(parent_index, std::move(widget_data));
      _widgets[widget_id].index     = widget_id;
      _widgets[widget_id].widget_id = IndexToWidget(_session_id, widget_id).id;
      widget = IndexToWidget(_session_id, widget_id);

      if (type && !strcmp(type, NEUI_W_MENUBAR)) {
        // MENUBAR: create the HMENU immediately; SetMenu is deferred to create_child_windows
        _widgets[widget_id].hmenu = CreateMenu();
        _menubars.push_back(widget_id);
        return widget;
      }

      // If parent HWND already exists, create child HWND immediately
      if (_widgets.exists(parent_index) && _widgets[parent_index].hwnd != nullptr) {
        UINT parent_dpi = get_dpi_for_widget(parent_index);
        _widgets[widget_id].hwnd = CreateChildHwnd(_widgets[widget_id], _widgets[parent_index].hwnd, parent_dpi);
      }
    }
    return widget;
  }

  void Session::widget_destroy(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;

    // Destroy children first (depth-first, leaves before parent) so every
    // descendant's ondestroy is called before the parent's.
    // Save next-sibling before each recursive call because the call removes
    // the child from the sibling list, invalidating _widgets.next().
    {
      uint32_t child_idx = _widgets.child(index);
      while (child_idx != 0) {
        uint32_t next = _widgets.next(child_idx);
        if (_widgets.exists(child_idx))
          widget_destroy({ _widgets[child_idx].widget_id });
        child_idx = next;
      }
    }

    auto& w = _widgets[index];
    if (_client_widget_api) {
      _client_widget_api->ondestroy(_token, widget, w.userdata);
    }
    if (w.has_subclass && w.hwnd) {
      RemoveWindowSubclass(w.hwnd, ChildSubclassProc, 1);
      w.has_subclass = false;
    }
    if (w.hwnd) {
      DestroyWindow(w.hwnd);
      w.hwnd = nullptr;
    }
    if (w.hfont) {
      DeleteObject(w.hfont);
      w.hfont = nullptr;
    }
    if (w.native_icon) {
      DestroyIcon(w.native_icon);
      w.native_icon = nullptr;
    }
    if (w.section_ctl_bg_brush) {
      DeleteObject(w.section_ctl_bg_brush);
      w.section_ctl_bg_brush      = nullptr;
      w.section_ctl_bg_brush_argb = 0;
    }
    // D2D bitmap and context are freed in ImageWndProc WM_DESTROY (triggered by
    // DestroyWindow above). Guard here in case the HWND was already gone.
    if (w.image_bitmap || w.image_ctx) {
      auto* backend = neui_d2d_backend::get_backend();
      if (backend) {
        if (w.image_bitmap) backend->destroy_bitmap(w.image_ctx, w.image_bitmap);
        if (w.image_ctx)    backend->destroy_context(w.image_ctx);
      }
      w.image_bitmap = nullptr;
      w.image_ctx    = nullptr;
    }
    if (w.hmenu) {
      DestroyMenu(w.hmenu);
      w.hmenu = nullptr;
      _menubars.erase(std::remove(_menubars.begin(), _menubars.end(), index), _menubars.end());
    }
    if (w.native_accel) {
      DestroyAcceleratorTable(w.native_accel);
      w.native_accel = nullptr;
    }
    _widgets.remove(index);
  }

  void Session::widget_show(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    w.visible = true;

    if (w.hwnd == nullptr && w.isroot) {
      // Deferred frame window creation on first show
      UINT initial_dpi = GetDpiForSystem();
      bool is_plug   = w.type && !strcmp(w.type, NEUI_W_PLUGWINDOW);
      bool is_dialog = w.type && !strcmp(w.type, NEUI_W_DIALOG);

      DWORD ex_style;
      DWORD style;
      const wchar_t* wclass;
      if (is_plug) {
        ex_style = 0;
        style    = WS_POPUP | WS_CLIPCHILDREN | WS_CLIPSIBLINGS;
        wclass   = L"neui.plugwindow";
      } else if (is_dialog) {
        ex_style = WS_EX_DLGMODALFRAME;
        style    = WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU;
        wclass   = L"neui.dialog";
      } else {
        ex_style = WS_EX_APPWINDOW;
        style    = WS_OVERLAPPEDWINDOW;
        wclass   = L"neui.appwindow";
      }

      // Resolve owner HWND for dialogs so CreateWindowEx can wire the
      // hwndParent slot correctly.
      HWND owner_hwnd = nullptr;
      if (is_dialog && w.owner_index != 0 && _widgets.exists(w.owner_index))
        owner_hwnd = _widgets[w.owner_index].hwnd;

      // Auto-centre the dialog over its owner when the client did not
      // request a specific position. (0, 0) is the conventional "no
      // position chosen" sentinel - clients that want a hard top-left
      // can call set_pos with explicit non-zero coords.
      int show_x_phys = LogicalToPhysical(w.x, initial_dpi);
      int show_y_phys = LogicalToPhysical(w.y, initial_dpi);
      int show_w_phys = LogicalToPhysical(w.width,  initial_dpi);
      int show_h_phys = LogicalToPhysical(w.height, initial_dpi);
      if (is_dialog && owner_hwnd && w.x == 0 && w.y == 0) {
        RECT or_rc;
        if (GetWindowRect(owner_hwnd, &or_rc)) {
          show_x_phys = or_rc.left + ((or_rc.right  - or_rc.left) - show_w_phys) / 2;
          show_y_phys = or_rc.top  + ((or_rc.bottom - or_rc.top)  - show_h_phys) / 2;
          if (show_x_phys < 0) show_x_phys = 0;
          if (show_y_phys < 0) show_y_phys = 0;
        }
      }

      CreateWindowExW(
        ex_style,
        wclass,
        L"",
        style,
        show_x_phys,
        show_y_phys,
        show_w_phys,
        show_h_phys,
        owner_hwnd,
        nullptr,
        get_hinstance(),
        &w  // retrieved in WM_NCCREATE via CREATESTRUCT::lpCreateParams
      );
      // w.hwnd is set by AppWindowProc WM_NCCREATE; children created in WM_CREATE
      if (w.hwnd) {
        if (!is_plug) ApplyText(w.hwnd, w.text);
        // Apply any icon attribute set before show.
        if (w.attrs) {
          const char* icon_path = w.attrs->get_string(NEUI_ATTR_ICON_PATH);
          if (icon_path && *icon_path)
            neui_detail::apply_window_icon(w.hwnd, icon_path, &w.native_icon);
        }
        ShowWindow(w.hwnd, SW_SHOWNORMAL);
        UpdateWindow(w.hwnd);

        // Dialog: block the owner's input while shown - unless the client
        // opted into modeless via NEUI_ATTR_MODAL = 0.
        bool is_modal = !w.attrs ||
                        w.attrs->get_int(NEUI_ATTR_MODAL, 1) != 0;
        if (is_dialog && owner_hwnd && is_modal)
          EnableWindow(owner_hwnd, FALSE);
      }
    } else if (w.hwnd != nullptr) {
      ShowWindow(w.hwnd, SW_SHOW);
    }
  }

  void Session::widget_hide(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    w.visible = false;
    if (w.hwnd) {
      ShowWindow(w.hwnd, SW_HIDE);
    }
  }

  void Session::widget_set_pos(neui_widget_t widget, int x, int y, int width, int height)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    w.x = x; w.y = y; w.width = width; w.height = height;
    if (w.hwnd) {
      UINT dpi = get_dpi_for_widget(index);
      SetWindowPos(w.hwnd, nullptr,
        LogicalToPhysical(x, dpi),
        LogicalToPhysical(y, dpi),
        LogicalToPhysical(width, dpi),
        LogicalToPhysical(height, dpi),
        SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }

  void Session::widget_set_size(neui_widget_t widget, int width, int height)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    w.width = width; w.height = height;
    if (w.hwnd) {
      UINT dpi = get_dpi_for_widget(index);
      SetWindowPos(w.hwnd, nullptr,
        0, 0,
        LogicalToPhysical(width, dpi),
        LogicalToPhysical(height, dpi),
        SWP_NOMOVE | SWP_NOZORDER | SWP_NOACTIVATE);
    }
  }

  void Session::widget_set_emit_events(neui_widget_t widget, bool enabled)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    _widgets[index].emit_events = enabled;
  }

  void Session::widget_set_text(neui_widget_t widget, const char* text)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    w.text = text ? text : "";
    if (w.type && !strcmp(w.type, NEUI_W_IMAGE)) {
      // For image widgets the text is the image filename - reload the bitmap.
      if (w.hwnd) load_image_bitmap(w, get_dpi_for_widget(index));
    } else if (w.type && !strcmp(w.type, NEUI_W_SECTION)) {
      // Section: text is the header title; chip width and the window
      // region both depend on it. Rebuild + repaint.
      if (w.hwnd) {
        apply_section_region_w32(w);
        InvalidateRect(w.hwnd, nullptr, FALSE);
      }
    } else {
      // Apply immediately if the HWND already exists; otherwise stored for deferred creation
      ApplyText(w.hwnd, w.text);
    }
  }

  int Session::widget_get_text(neui_widget_t widget, char* buf, int buflen)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return -1;
    auto& w = _widgets[index];

    // Sync from the Win32 control - captures user edits in INPUTBOX
    if (w.hwnd) {
      int wlen = GetWindowTextLengthW(w.hwnd);
      if (wlen > 0) {
        std::wstring wtext(wlen, L'\0');
        GetWindowTextW(w.hwnd, &wtext[0], wlen + 1);
        w.text = FromWide(wtext.c_str(), wlen);
      } else {
        w.text.clear();
      }
    }

    int needed = static_cast<int>(w.text.size()) + 1;  // bytes including null terminator
    if (buf && buflen > 0) {
      int copy = static_cast<int>(std::min(w.text.size(), static_cast<size_t>(buflen - 1)));
      memcpy(buf, w.text.c_str(), copy);
      buf[copy] = '\0';
    }
    return needed;
  }

  // Static C API wrapper functions

  static neui_widget_t create(neui_session_t session, neui_widget_t parent, const char* type, int x, int y, int width, int height, void* userdata)
  {
    auto s = get_session_for_widget(session, parent);
    if (s) return s->widget_create(parent, type, x, y, width, height, userdata);
    return { 0xFFFFFFFF };
  }

  static void destroy(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_destroy(widget);
  }

  static void show(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_show(widget);
  }

  static void hide(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_hide(widget);
  }

  static void set_pos(neui_session_t session, neui_widget_t widget, int x, int y, int width, int height)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_set_pos(widget, x, y, width, height);
  }

  static void set_size(neui_session_t session, neui_widget_t widget, int width, int height)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_set_size(widget, width, height);
  }

  static void set_emit_events(neui_session_t session, neui_widget_t widget, bool enabled)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_set_emit_events(widget, enabled);
  }

  neui_widget_t Session::widget_get_first_child(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return widget_none;
    uint32_t child = _widgets.child(index);
    if (child == 0 || !_widgets.exists(child)) return widget_none;
    return { _widgets[child].widget_id };
  }

  neui_widget_t Session::widget_get_next_sibling(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return widget_none;
    uint32_t next = _widgets.next(index);
    if (next == 0 || !_widgets.exists(next)) return widget_none;
    return { _widgets[next].widget_id };
  }

  static void set_text(neui_session_t session, neui_widget_t widget, const char* text)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_set_text(widget, text);
  }

  static int get_text(neui_session_t session, neui_widget_t widget, char* buf, int buflen)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) return s->widget_get_text(widget, buf, buflen);
    return -1;
  }

  static neui_widget_t get_first_child(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) return s->widget_get_first_child(widget);
    return widget_none;
  }

  static neui_widget_t get_next_sibling(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) return s->widget_get_next_sibling(widget);
    return widget_none;
  }

  void Session::widget_set_focus(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    if (w.hwnd) {
      SetFocus(w.hwnd);
    }
  }

  void Session::widget_set_owner(neui_widget_t dialog, neui_widget_t owner)
  {
    uint32_t didx = WidgetToIndex(dialog);
    if (!_widgets.exists(didx)) return;
    auto& d = _widgets[didx];
    if (!d.type || strcmp(d.type, NEUI_W_DIALOG) != 0) return;

    if (owner.id == widget_none.id) {
      d.owner_index = 0;
      return;
    }
    uint32_t oidx = WidgetToIndex(owner);
    if (!_widgets.exists(oidx)) return;
    const char* ot = _widgets[oidx].type;
    if (!ot || (strcmp(ot, NEUI_W_APPWINDOW) != 0 &&
                strcmp(ot, NEUI_W_PLUGWINDOW) != 0 &&
                strcmp(ot, NEUI_W_DIALOG)    != 0))
      return;
    d.owner_index = oidx;
  }

  void Session::close_owned_frames(uint32_t owner_idx)
  {
    if (owner_idx == 0) return;
    for (uint32_t idx : _widgets.release_order()) {
      if (idx == 0 || idx == owner_idx) continue;
      if (!_widgets.exists(idx)) continue;
      auto& other = _widgets[idx];
      if (other.owner_index == owner_idx && other.hwnd) {
        HWND child_hwnd = other.hwnd;
        other.hwnd = nullptr;
        DestroyWindow(child_hwnd);
      }
    }
  }

  void Session::update_menu_popup(HMENU popup)
  {
    if (!popup) return;
    bool has_validate = _menu_client && _menu_client->validate;
    for (uint32_t mb_idx : _menubars) {
      if (!_widgets.exists(mb_idx)) continue;
      auto& mw = _widgets[mb_idx];
      for (auto& kv : mw.menu_items) {
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
          enabled = can_focused_perform_command(mi.menu_cmd);
        }
        if (enabled && has_validate) {
          enabled = _menu_client->validate(
            _token, { mw.widget_id }, { neui_id }, mi.menu_cmd);
        }
        EnableMenuItem(popup, mi.cmd_id,
                        MF_BYCOMMAND | (enabled ? MF_ENABLED : MF_GRAYED));
      }
    }
  }

  HWND Session::find_parent_hwnd(uint32_t widget_index)
  {
    auto parents = _widgets.get_all_parents(widget_index);
    for (auto p : parents) {
      if (p == 0) continue;
      if (_widgets.exists(p) && _widgets[p].hwnd)
        return _widgets[p].hwnd;
    }
    return nullptr;
  }

  bool Session::dispatch_menu_event(UINT cmd_id)
  {
    for (uint32_t mb_idx : _menubars) {
      if (!_widgets.exists(mb_idx)) continue;
      auto& mb = _widgets[mb_idx];
      auto it = mb.menu_cmd_map.find(cmd_id);
      if (it == mb.menu_cmd_map.end()) continue;
      uint32_t neui_id = it->second;

      // Built-in command routing: ask the focused control first.
      auto data_it = mb.menu_items.find(neui_id);
      if (data_it != mb.menu_items.end()) {
        uint32_t cmd = data_it->second.menu_cmd;
        if (cmd != 0 && cmd < NEUI_CMD_USER_BASE) {
          if (invoke_focused_command(cmd)) return true;
        }
      }

      neui_widget_t wid = { mb.widget_id };
      neui_event_t  ev  = { NEUI_EVENT_TREE_ITEM_ACTIVATED };
      ev.data.tree = { wid, { neui_id } };
      return dispatch_event(&ev);
    }
    return false;
  }

  // -------------------------------------------------------------------------
  // Commands API - uses GetFocus() to find the native edit and sends the
  // standard EM_*/WM_* messages. Other widget types are no-ops on Win32.

  // Returns true if hwnd is a native Edit control that knows how to handle
  // `cmd` - used both for the actual send and for the can-perform query.
  // Native Edit's EM_UNDO is itself a toggle (per MSDN: "the undo
  // operation can also be undone"), so REDO maps to EM_UNDO too: pressing
  // Ctrl+Y immediately after Ctrl+Z replays the edit. Limitation: undo is
  // single-level, so pressing Ctrl+Z twice toggles the same unit; clients
  // that need multi-level redo should use the xpl host's text widgets,
  // which have a full EditHistory.
  static bool edit_can_handle(HWND hwnd, uint32_t cmd)
  {
    if (!hwnd) return false;
    wchar_t cls[32] = {};
    GetClassNameW(hwnd, cls, 32);
    if (lstrcmpiW(cls, L"Edit") != 0) return false;
    switch (cmd) {
    case NEUI_CMD_UNDO:
    case NEUI_CMD_REDO:
    case NEUI_CMD_CUT:
    case NEUI_CMD_COPY:
    case NEUI_CMD_PASTE:
    case NEUI_CMD_SELECT_ALL:
    case NEUI_CMD_DELETE:
      return true;
    }
    return false;
  }

  static bool send_text_command(HWND hwnd, uint32_t cmd)
  {
    if (!edit_can_handle(hwnd, cmd)) return false;
    switch (cmd) {
    case NEUI_CMD_UNDO:       SendMessageW(hwnd, EM_UNDO,    0, 0);      return true;
    case NEUI_CMD_REDO:       SendMessageW(hwnd, EM_UNDO,    0, 0);      return true;
    case NEUI_CMD_CUT:        SendMessageW(hwnd, WM_CUT,     0, 0);      return true;
    case NEUI_CMD_COPY:       SendMessageW(hwnd, WM_COPY,    0, 0);      return true;
    case NEUI_CMD_PASTE:      SendMessageW(hwnd, WM_PASTE,   0, 0);      return true;
    case NEUI_CMD_SELECT_ALL: SendMessageW(hwnd, EM_SETSEL,  0, -1);     return true;
    case NEUI_CMD_DELETE:     SendMessageW(hwnd, WM_CLEAR,   0, 0);      return true;
    }
    return false;
  }

  bool Session::invoke_focused_command(uint32_t cmd)
  {
    if (cmd == NEUI_CMD_NONE || cmd >= NEUI_CMD_USER_BASE) return false;
    return send_text_command(GetFocus(), cmd);
  }

  bool Session::invoke_command(neui_widget_t widget, uint32_t cmd)
  {
    if (cmd == NEUI_CMD_NONE || cmd >= NEUI_CMD_USER_BASE) return false;
    uint32_t idx = WidgetToIndex(widget);
    auto* w = get_widget(idx);
    if (!w) return false;
    return send_text_command(w->hwnd, cmd);
  }

  bool Session::can_focused_perform_command(uint32_t cmd)
  {
    if (cmd == NEUI_CMD_NONE || cmd >= NEUI_CMD_USER_BASE) return false;
    return edit_can_handle(GetFocus(), cmd);
  }

  static int NEUI_ABI cmd_invoke_focused(neui_session_t session, uint32_t cmd)
  {
    auto s = get_session(session);
    return (s && s->invoke_focused_command(cmd)) ? 1 : 0;
  }

  static int NEUI_ABI cmd_invoke(neui_session_t session, neui_widget_t widget,
                                  uint32_t cmd)
  {
    auto s = get_session_for_widget(session, widget);
    return (s && s->invoke_command(widget, cmd)) ? 1 : 0;
  }

  neui_commands_api_t commands_api = {
    NEUI_VERSION,
    cmd_invoke_focused,
    cmd_invoke,
  };

  // ---------------------------------------------------------------------------
  // Items API implementation
  // ---------------------------------------------------------------------------

  void Session::items_clear(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    auto* ops = get_items_ops(w.type);
    if (!ops || !w.hwnd) return;
    SendMessageW(w.hwnd, ops->clear, 0, 0);
  }

  uint32_t Session::items_add(neui_widget_t widget, const char* text, void* userdata)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return NEUI_ITEM_NONE;
    auto& w = _widgets[index];
    auto* ops = get_items_ops(w.type);
    if (!ops) return NEUI_ITEM_NONE;
    if (!w.hwnd) {
      // HWND not yet created - buffer for deferred application on show
      w.pending_items.push_back({ text ? text : "", userdata });
      return static_cast<uint32_t>(w.pending_items.size() - 1);
    }
    auto wtext = ToWide(text ? text : "");
    LRESULT idx = SendMessageW(w.hwnd, ops->add, 0, (LPARAM)wtext.c_str());
    if (idx < 0) return NEUI_ITEM_NONE;
    SendMessageW(w.hwnd, ops->setdata, (WPARAM)idx, (LPARAM)userdata);
    return static_cast<uint32_t>(idx);
  }

  void Session::items_remove(neui_widget_t widget, uint32_t index)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];
    auto* ops = get_items_ops(w.type);
    if (!ops || !w.hwnd) return;
    SendMessageW(w.hwnd, ops->del, (WPARAM)index, 0);
  }

  uint32_t Session::items_count(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return 0;
    auto& w = _widgets[index];
    auto* ops = get_items_ops(w.type);
    if (!ops || !w.hwnd) return 0;
    LRESULT n = SendMessageW(w.hwnd, ops->count, 0, 0);
    return (n < 0) ? 0 : static_cast<uint32_t>(n);
  }

  int Session::items_get_text(neui_widget_t widget, uint32_t index, char* buf, int buflen)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return -1;
    auto& w = _widgets[wi];
    auto* ops = get_items_ops(w.type);
    if (!ops || !w.hwnd) return -1;
    LRESULT wlen = SendMessageW(w.hwnd, ops->textlen, (WPARAM)index, 0);
    if (wlen < 0) return -1;
    std::wstring wbuf(static_cast<size_t>(wlen), L'\0');
    SendMessageW(w.hwnd, ops->gettext, (WPARAM)index, (LPARAM)&wbuf[0]);
    std::string utf8 = FromWide(wbuf.c_str(), static_cast<int>(wlen));
    int needed = static_cast<int>(utf8.size()) + 1;
    if (buf && buflen > 0) {
      int copy = static_cast<int>(std::min(utf8.size(), static_cast<size_t>(buflen - 1)));
      memcpy(buf, utf8.c_str(), copy);
      buf[copy] = '\0';
    }
    return needed;
  }

  void Session::items_set_text(neui_widget_t widget, uint32_t index, const char* text)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];
    auto* ops = get_items_ops(w.type);
    if (!ops || !w.hwnd) return;
    // Preserve userdata across delete+reinsert
    LRESULT udata = SendMessageW(w.hwnd, ops->getdata, (WPARAM)index, 0);
    SendMessageW(w.hwnd, ops->del, (WPARAM)index, 0);
    auto wtext = ToWide(text ? text : "");
    SendMessageW(w.hwnd, ops->insert, (WPARAM)index, (LPARAM)wtext.c_str());
    SendMessageW(w.hwnd, ops->setdata, (WPARAM)index, (LPARAM)udata);
  }

  void* Session::items_get_userdata(neui_widget_t widget, uint32_t index)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return nullptr;
    auto& w = _widgets[wi];
    auto* ops = get_items_ops(w.type);
    if (!ops || !w.hwnd) return nullptr;
    return reinterpret_cast<void*>(SendMessageW(w.hwnd, ops->getdata, (WPARAM)index, 0));
  }

  uint32_t Session::items_get_selected(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return NEUI_ITEM_NONE;
    auto& w = _widgets[index];
    auto* ops = get_items_ops(w.type);
    if (!ops || !w.hwnd) return NEUI_ITEM_NONE;
    LRESULT sel = SendMessageW(w.hwnd, ops->getsel, 0, 0);
    return (sel < 0) ? NEUI_ITEM_NONE : static_cast<uint32_t>(sel);
  }

  void Session::items_set_selected(neui_widget_t widget, uint32_t index)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];
    auto* ops = get_items_ops(w.type);
    if (!ops) return;
    if (!w.hwnd) {
      w.pending_selection = index;
      return;
    }
    WPARAM item = (index == NEUI_ITEM_NONE) ? (WPARAM)-1 : (WPARAM)index;
    SendMessageW(w.hwnd, ops->setsel, item, 0);
  }

  // ---------------------------------------------------------------------------
  // Items API static wrappers
  // ---------------------------------------------------------------------------

  static void items_clear_fn(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->items_clear(widget);
  }
  static uint32_t items_add_fn(neui_session_t session, neui_widget_t widget, const char* text, void* userdata)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->items_add(widget, text, userdata) : NEUI_ITEM_NONE;
  }
  static void items_remove_fn(neui_session_t session, neui_widget_t widget, uint32_t index)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->items_remove(widget, index);
  }
  static uint32_t items_count_fn(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->items_count(widget) : 0;
  }
  static int items_get_text_fn(neui_session_t session, neui_widget_t widget, uint32_t index, char* buf, int buflen)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->items_get_text(widget, index, buf, buflen) : -1;
  }
  static void items_set_text_fn(neui_session_t session, neui_widget_t widget, uint32_t index, const char* text)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->items_set_text(widget, index, text);
  }
  static void* items_get_userdata_fn(neui_session_t session, neui_widget_t widget, uint32_t index)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->items_get_userdata(widget, index) : nullptr;
  }
  static uint32_t items_get_selected_fn(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->items_get_selected(widget) : NEUI_ITEM_NONE;
  }
  static void items_set_selected_fn(neui_session_t session, neui_widget_t widget, uint32_t index)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->items_set_selected(widget, index);
  }

  neui_items_api_t items_api = {
    items_clear_fn,
    items_add_fn,
    items_remove_fn,
    items_count_fn,
    items_get_text_fn,
    items_set_text_fn,
    items_get_userdata_fn,
    items_get_selected_fn,
    items_set_selected_fn,
  };

  // ---------------------------------------------------------------------------
  // Tree API implementation
  // ---------------------------------------------------------------------------

  static bool is_treeview(const char* type) { return type && !strcmp(type, NEUI_W_TREEVIEW); }
  static bool is_menubar (const char* type) { return type && !strcmp(type, NEUI_W_MENUBAR);  }

  neui_item_t Session::tree_add(neui_widget_t widget, neui_item_t parent, const char* text, void* userdata)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return tree_item_none;
    auto& w = _widgets[wi];
    const char* txt = text ? text : "";

    if (is_treeview(w.type)) {
      uint32_t neui_id = w.next_tree_id++;
      WidgetData::TreeItemData data;
      data.userdata = userdata;
      data.text     = txt;
      data.enabled  = true;
      data.hitem    = nullptr;

      if (!w.hwnd) {
        // Buffer until HWND is created
        w.tree_items[neui_id] = std::move(data);
        w.pending_tree_items.push_back({ neui_id, parent.id });
        return { neui_id };
      }
      HTREEITEM parent_hitem = TVI_ROOT;
      if (parent.id != 0) {
        auto it = w.tree_items.find(parent.id);
        if (it != w.tree_items.end()) parent_hitem = it->second.hitem;
      }
      TVINSERTSTRUCTW tvis   = {};
      tvis.hParent           = parent_hitem;
      tvis.hInsertAfter      = TVI_LAST;
      tvis.item.mask         = TVIF_TEXT | TVIF_PARAM;
      auto wtext             = ToWide(txt);
      tvis.item.pszText      = const_cast<LPWSTR>(wtext.c_str());
      tvis.item.lParam       = static_cast<LPARAM>(neui_id);
      HTREEITEM hitem        = (HTREEITEM)SendMessageW(w.hwnd, TVM_INSERTITEMW, 0, (LPARAM)&tvis);
      data.hitem             = hitem;
      w.tree_items[neui_id]  = std::move(data);
      if (hitem) w.tree_items_reverse[reinterpret_cast<uintptr_t>(hitem)] = neui_id;
      return { neui_id };
    }

    if (is_menubar(w.type)) {
      uint32_t neui_id = w.next_menu_item_id++;
      WidgetData::MenuItemData data;
      data.text           = txt;
      data.enabled        = true;
      data.userdata       = userdata;
      data.parent_item_id = parent.id;

      if (parent.id == 0) {
        // Top-level menu bar item - create a popup submenu for it
        HMENU popup = CreatePopupMenu();
        data.parent_hmenu  = w.hmenu;
        data.submenu       = popup;
        data.cmd_id        = 0;
        auto wtext         = ToWide(txt);
        AppendMenuW(w.hmenu, MF_POPUP, reinterpret_cast<UINT_PTR>(popup), wtext.c_str());
      } else {
        auto pit = w.menu_items.find(parent.id);
        HMENU parent_popup = (pit != w.menu_items.end() && pit->second.submenu) ? pit->second.submenu : w.hmenu;
        UINT  cmd_id       = w.next_menu_cmd_id++;
        data.parent_hmenu  = parent_popup;
        data.submenu       = nullptr;
        data.cmd_id        = cmd_id;

        if (strcmp(txt, "-") == 0) {
          // Separator - use InsertMenuItemW with MIIM_ID so cmd_id is stored and MF_BYCOMMAND removal works
          data.is_separator = true;
          MENUITEMINFOW mii = {};
          mii.cbSize = sizeof(mii);
          mii.fMask  = MIIM_TYPE | MIIM_ID;
          mii.fType  = MFT_SEPARATOR;
          mii.wID    = cmd_id;
          InsertMenuItemW(parent_popup, GetMenuItemCount(parent_popup), TRUE, &mii);
        } else {
          // Normal command item
          auto wtext = ToWide(txt);
          AppendMenuW(parent_popup, MF_STRING, cmd_id, wtext.c_str());
          w.menu_cmd_map[cmd_id] = neui_id;
        }
      }
      w.menu_items[neui_id] = std::move(data);
      w.menu_item_ids_ordered.push_back(neui_id);
      // If menu bar is already attached, refresh it
      HWND frame = find_parent_hwnd(wi);
      if (frame) DrawMenuBar(frame);
      return { neui_id };
    }
    return tree_item_none;
  }

  // Forward decl - body is below near tree_set_shortcut.
  static void rebuild_menubar_accel_w32(WidgetData& mb);

  void Session::tree_remove(neui_widget_t widget, neui_item_t item)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];

    if (is_treeview(w.type)) {
      auto it = w.tree_items.find(item.id);
      if (it == w.tree_items.end()) return;
      if (w.hwnd && it->second.hitem) {
        // TreeView_DeleteItem also removes children
        w.tree_items_reverse.erase(reinterpret_cast<uintptr_t>(it->second.hitem));
        TreeView_DeleteItem(w.hwnd, it->second.hitem);
      }
      w.tree_items.erase(it);
      return;
    }

    if (is_menubar(w.type)) {
      auto it = w.menu_items.find(item.id);
      if (it == w.menu_items.end()) return;
      auto& data = it->second;
      bool had_shortcut = (data.shortcut_key != NEUI_KEY_NONE);
      if (data.submenu) {
        RemoveMenu(data.parent_hmenu, reinterpret_cast<UINT_PTR>(data.submenu), MF_BYCOMMAND);
        DestroyMenu(data.submenu);
      } else {
        RemoveMenu(data.parent_hmenu, data.cmd_id, MF_BYCOMMAND);
        w.menu_cmd_map.erase(data.cmd_id);
      }
      w.menu_item_ids_ordered.erase(
        std::remove(w.menu_item_ids_ordered.begin(), w.menu_item_ids_ordered.end(), item.id),
        w.menu_item_ids_ordered.end());
      w.menu_items.erase(it);
      if (had_shortcut) rebuild_menubar_accel_w32(w);
      HWND frame = find_parent_hwnd(wi);
      if (frame) DrawMenuBar(frame);
    }
  }

  void Session::tree_clear(neui_widget_t widget)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];

    if (is_treeview(w.type)) {
      if (w.hwnd) TreeView_DeleteAllItems(w.hwnd);
      w.tree_items.clear();
      w.tree_items_reverse.clear();
      w.pending_tree_items.clear();
      w.next_tree_id = 1;
      return;
    }

    if (is_menubar(w.type)) {
      // Destroy and recreate the menu
      for (auto& pair : w.menu_items) {
        if (pair.second.submenu) DestroyMenu(pair.second.submenu);
      }
      w.menu_items.clear();
      w.menu_cmd_map.clear();
      w.menu_item_ids_ordered.clear();
      w.next_menu_item_id = 1;
      w.next_menu_cmd_id  = 0x8000;
      if (w.native_accel) {
        DestroyAcceleratorTable(w.native_accel);
        w.native_accel = nullptr;
      }
      // Repopulate with a fresh menu
      DestroyMenu(w.hmenu);
      w.hmenu = CreateMenu();
      HWND frame = find_parent_hwnd(wi);
      if (frame) { SetMenu(frame, w.hmenu); DrawMenuBar(frame); }
    }
  }

  int Session::tree_get_text(neui_widget_t widget, neui_item_t item, char* buf, int buflen)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return -1;
    auto& w = _widgets[wi];

    if (is_treeview(w.type)) {
      auto it = w.tree_items.find(item.id);
      if (it == w.tree_items.end()) return -1;
      // Sync from the Win32 control if HWND exists
      if (w.hwnd && it->second.hitem) {
        wchar_t wbuf[512] = {};
        TVITEMW tvi = {};
        tvi.hItem      = it->second.hitem;
        tvi.mask       = TVIF_TEXT;
        tvi.pszText    = wbuf;
        tvi.cchTextMax = 512;
        SendMessageW(w.hwnd, TVM_GETITEMW, 0, (LPARAM)&tvi);
        it->second.text = FromWide(wbuf, static_cast<int>(wcslen(wbuf)));
      }
      const std::string& s = it->second.text;
      int needed = static_cast<int>(s.size()) + 1;
      if (buf && buflen > 0) {
        int copy = static_cast<int>(std::min(s.size(), static_cast<size_t>(buflen - 1)));
        memcpy(buf, s.c_str(), copy);
        buf[copy] = '\0';
      }
      return needed;
    }

    if (is_menubar(w.type)) {
      auto it = w.menu_items.find(item.id);
      if (it == w.menu_items.end()) return -1;
      const std::string s = it->second.is_separator ? std::string("-") : it->second.text;
      int needed = static_cast<int>(s.size()) + 1;
      if (buf && buflen > 0) {
        int copy = static_cast<int>(std::min(s.size(), static_cast<size_t>(buflen - 1)));
        memcpy(buf, s.c_str(), copy);
        buf[copy] = '\0';
      }
      return needed;
    }
    return -1;
  }

  void Session::tree_set_text(neui_widget_t widget, neui_item_t item, const char* text)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];
    const char* txt = text ? text : "";

    if (is_treeview(w.type)) {
      auto it = w.tree_items.find(item.id);
      if (it == w.tree_items.end()) return;
      it->second.text = txt;
      if (w.hwnd && it->second.hitem) {
        auto wtext = ToWide(txt);
        TVITEMW tvi = {};
        tvi.hItem    = it->second.hitem;
        tvi.mask     = TVIF_TEXT;
        tvi.pszText  = const_cast<LPWSTR>(wtext.c_str());
        SendMessageW(w.hwnd, TVM_SETITEMW, 0, (LPARAM)&tvi);
      }
      return;
    }

    if (is_menubar(w.type)) {
      auto it = w.menu_items.find(item.id);
      if (it == w.menu_items.end()) return;
      it->second.text = txt;
      std::string display = txt;
      if (!it->second.shortcut.empty()) display += '\t' + it->second.shortcut;
      auto wtext = ToWide(display.c_str());
      UINT flags = (it->second.submenu ? MF_POPUP : MF_STRING);
      UINT_PTR id_or_menu = it->second.submenu ? reinterpret_cast<UINT_PTR>(it->second.submenu)
                                                : static_cast<UINT_PTR>(it->second.cmd_id);
      ModifyMenuW(it->second.parent_hmenu, id_or_menu, flags | MF_BYCOMMAND, id_or_menu, wtext.c_str());
      HWND frame = find_parent_hwnd(wi);
      if (frame) DrawMenuBar(frame);
    }
  }

  void* Session::tree_get_userdata(neui_widget_t widget, neui_item_t item)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return nullptr;
    auto& w = _widgets[wi];

    if (is_treeview(w.type)) {
      auto it = w.tree_items.find(item.id);
      return (it != w.tree_items.end()) ? it->second.userdata : nullptr;
    }
    if (is_menubar(w.type)) {
      auto it = w.menu_items.find(item.id);
      return (it != w.menu_items.end()) ? it->second.userdata : nullptr;
    }
    return nullptr;
  }

  void Session::tree_set_enabled(neui_widget_t widget, neui_item_t item, bool enabled)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];

    if (is_treeview(w.type)) {
      auto it = w.tree_items.find(item.id);
      if (it != w.tree_items.end()) it->second.enabled = enabled;
      return;
    }
    if (is_menubar(w.type)) {
      auto it = w.menu_items.find(item.id);
      if (it == w.menu_items.end() || it->second.is_separator) return;
      it->second.enabled = enabled;
      UINT flags = enabled ? MF_ENABLED : MF_GRAYED;
      if (it->second.submenu) {
        // Popup item: find its position in the parent menu (HMENU is 64-bit; can't use MF_BYCOMMAND with UINT)
        int count = GetMenuItemCount(it->second.parent_hmenu);
        for (int pos = 0; pos < count; ++pos) {
          if (GetSubMenu(it->second.parent_hmenu, pos) == it->second.submenu) {
            EnableMenuItem(it->second.parent_hmenu, static_cast<UINT>(pos), MF_BYPOSITION | flags);
            break;
          }
        }
      } else {
        EnableMenuItem(it->second.parent_hmenu, it->second.cmd_id, MF_BYCOMMAND | flags);
      }
      HWND frame = find_parent_hwnd(wi);
      if (frame) DrawMenuBar(frame);
    }
  }

  bool Session::tree_get_enabled(neui_widget_t widget, neui_item_t item)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return true;
    auto& w = _widgets[wi];

    if (is_treeview(w.type)) {
      auto it = w.tree_items.find(item.id);
      return (it != w.tree_items.end()) ? it->second.enabled : true;
    }
    if (is_menubar(w.type)) {
      auto it = w.menu_items.find(item.id);
      return (it != w.menu_items.end()) ? it->second.enabled : true;
    }
    return true;
  }

  bool Session::try_translate_accel(MSG* msg)
  {
    if (!msg) return false;
    HWND root = msg->hwnd ? GetAncestor(msg->hwnd, GA_ROOT) : nullptr;
    if (!root) return false;
    for (uint32_t mb_idx : _menubars) {
      if (!_widgets.exists(mb_idx)) continue;
      auto& mb = _widgets[mb_idx];
      if (!mb.native_accel) continue;
      HWND frame = find_parent_hwnd(mb_idx);
      if (!frame || frame != root) continue;
      if (TranslateAcceleratorW(frame, mb.native_accel, msg)) return true;
    }
    return false;
  }

  // Rebuild the menubar's HACCEL from its current item shortcuts.
  static void rebuild_menubar_accel_w32(WidgetData& mb)
  {
    std::vector<neui_detail::AccelEntry> entries;
    entries.reserve(mb.menu_items.size());
    for (const auto& kv : mb.menu_items) {
      const auto& d = kv.second;
      if (d.submenu || d.is_separator) continue;
      if (d.shortcut_key != NEUI_KEY_NONE)
        entries.push_back({ d.shortcut_mods, d.shortcut_key, d.cmd_id });
      // Standard platform-alias shortcuts (e.g. Ctrl+Shift+Z for REDO).
      neui_detail::append_builtin_command_aliases(
        d.menu_cmd, d.shortcut_mods, d.shortcut_key, d.cmd_id, entries);
    }
    HACCEL old = mb.native_accel;
    mb.native_accel = neui_detail::build_accel_table(entries);
    if (old) DestroyAcceleratorTable(old);
  }

  void Session::tree_set_shortcut(neui_widget_t widget, neui_item_t item,
                                   uint32_t modifiers, uint32_t key)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];

    if (is_menubar(w.type)) {
      auto it = w.menu_items.find(item.id);
      if (it == w.menu_items.end()) return;
      it->second.shortcut_mods = modifiers;
      it->second.shortcut_key  = key;
      it->second.shortcut      =
        neui_detail::format_shortcut_label_win(modifiers, key);
      // Re-apply text with the new shortcut hint, then rebuild the accel table.
      tree_set_text(widget, item, it->second.text.c_str());
      rebuild_menubar_accel_w32(w);
    }
    // For treeview, shortcut hint is ignored.
  }

  neui_item_t Session::tree_get_first_child(neui_widget_t widget, neui_item_t parent)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return tree_item_none;
    auto& w = _widgets[wi];

    if (is_treeview(w.type) && w.hwnd) {
      HTREEITEM parent_hitem = (parent.id == 0) ? TVI_ROOT : nullptr;
      if (parent.id != 0) {
        auto it = w.tree_items.find(parent.id);
        if (it == w.tree_items.end()) return tree_item_none;
        parent_hitem = it->second.hitem;
      }
      HTREEITEM child = TreeView_GetChild(w.hwnd, parent_hitem);
      if (!child) return tree_item_none;
      auto it = w.tree_items_reverse.find(reinterpret_cast<uintptr_t>(child));
      return (it != w.tree_items_reverse.end()) ? neui_item_t{ it->second } : tree_item_none;
    }

    if (is_menubar(w.type)) {
      for (uint32_t id : w.menu_item_ids_ordered) {
        auto it = w.menu_items.find(id);
        if (it != w.menu_items.end() && it->second.parent_item_id == parent.id)
          return { id };
      }
    }
    return tree_item_none;
  }

  neui_item_t Session::tree_get_next_sibling(neui_widget_t widget, neui_item_t item)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return tree_item_none;
    auto& w = _widgets[wi];

    if (is_treeview(w.type) && w.hwnd) {
      auto it = w.tree_items.find(item.id);
      if (it == w.tree_items.end() || !it->second.hitem) return tree_item_none;
      HTREEITEM next = TreeView_GetNextSibling(w.hwnd, it->second.hitem);
      if (!next) return tree_item_none;
      auto rit = w.tree_items_reverse.find(reinterpret_cast<uintptr_t>(next));
      return (rit != w.tree_items_reverse.end()) ? neui_item_t{ rit->second } : tree_item_none;
    }

    if (is_menubar(w.type)) {
      auto cur = w.menu_items.find(item.id);
      if (cur == w.menu_items.end()) return tree_item_none;
      uint32_t parent_id = cur->second.parent_item_id;
      bool found = false;
      for (uint32_t id : w.menu_item_ids_ordered) {
        if (id == item.id) { found = true; continue; }
        if (!found) continue;
        auto it = w.menu_items.find(id);
        if (it != w.menu_items.end() && it->second.parent_item_id == parent_id)
          return { id };
      }
    }
    return tree_item_none;
  }

  neui_item_t Session::tree_get_selected(neui_widget_t widget)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return tree_item_none;
    auto& w = _widgets[wi];

    if (is_treeview(w.type) && w.hwnd) {
      HTREEITEM sel = TreeView_GetSelection(w.hwnd);
      if (!sel) return tree_item_none;
      auto it = w.tree_items_reverse.find(reinterpret_cast<uintptr_t>(sel));
      return (it != w.tree_items_reverse.end()) ? neui_item_t{ it->second } : tree_item_none;
    }
    return tree_item_none;  // menu bar: no persistent selection
  }

  void Session::tree_set_selected(neui_widget_t widget, neui_item_t item)
  {
    uint32_t wi = WidgetToIndex(widget);
    if (!_widgets.exists(wi)) return;
    auto& w = _widgets[wi];

    if (is_treeview(w.type) && w.hwnd) {
      auto it = w.tree_items.find(item.id);
      if (it != w.tree_items.end() && it->second.hitem)
        TreeView_SelectItem(w.hwnd, it->second.hitem);
    }
    // menu bar: no-op
  }

  // ---------------------------------------------------------------------------
  // Tree API static wrappers
  // ---------------------------------------------------------------------------

  static neui_item_t tree_add_fn(neui_session_t session, neui_widget_t widget,
                                  neui_item_t parent, const char* text, void* userdata)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_add(widget, parent, text, userdata) : tree_item_none;
  }
  static void tree_remove_fn(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->tree_remove(widget, item);
  }
  static void tree_clear_fn(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->tree_clear(widget);
  }
  static int tree_get_text_fn(neui_session_t session, neui_widget_t widget, neui_item_t item, char* buf, int buflen)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_get_text(widget, item, buf, buflen) : -1;
  }
  static void tree_set_text_fn(neui_session_t session, neui_widget_t widget, neui_item_t item, const char* text)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->tree_set_text(widget, item, text);
  }
  static void* tree_get_userdata_fn(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_get_userdata(widget, item) : nullptr;
  }
  static void tree_set_enabled_fn(neui_session_t session, neui_widget_t widget, neui_item_t item, bool enabled)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->tree_set_enabled(widget, item, enabled);
  }
  static bool tree_get_enabled_fn(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_get_enabled(widget, item) : true;
  }
  static void tree_set_shortcut_fn(neui_session_t session, neui_widget_t widget, neui_item_t item,
                                    uint32_t modifiers, uint32_t key)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->tree_set_shortcut(widget, item, modifiers, key);
  }
  static neui_item_t tree_get_first_child_fn(neui_session_t session, neui_widget_t widget, neui_item_t parent)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_get_first_child(widget, parent) : tree_item_none;
  }
  static neui_item_t tree_get_next_sibling_fn(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_get_next_sibling(widget, item) : tree_item_none;
  }
  static neui_item_t tree_get_selected_fn(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget); return s ? s->tree_get_selected(widget) : tree_item_none;
  }
  static void tree_set_selected_fn(neui_session_t session, neui_widget_t widget, neui_item_t item)
  {
    auto s = get_session_for_widget(session, widget); if (s) s->tree_set_selected(widget, item);
  }
  static void tree_set_menu_cmd_fn(neui_session_t session, neui_widget_t widget,
                                    neui_item_t item, uint32_t command)
  {
    auto s = get_session_for_widget(session, widget); if (!s) return;
    uint32_t wi = WidgetToIndex(widget);
    auto* w = s->get_widget(wi);
    if (!w || !is_menubar(w->type)) return;
    auto it = w->menu_items.find(item.id);
    if (it != w->menu_items.end()) it->second.menu_cmd = command;
  }

  neui_tree_api_t tree_api = {
    tree_add_fn,
    tree_remove_fn,
    tree_clear_fn,
    tree_get_text_fn,
    tree_set_text_fn,
    tree_get_userdata_fn,
    tree_set_enabled_fn,
    tree_get_enabled_fn,
    tree_set_shortcut_fn,
    tree_get_first_child_fn,
    tree_get_next_sibling_fn,
    tree_get_selected_fn,
    tree_set_selected_fn,
    tree_set_menu_cmd_fn,
  };

  static void set_focus(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_set_focus(widget);
  }

  static bool is_checkbox(const char* type) {
    return type && (!strcmp(type, NEUI_W_CHECKBOX) || !strcmp(type, NEUI_W_CHECKBOX3));
  }

  void Session::widget_set_check(neui_widget_t widget, neui_check_state_t state)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return;
    auto& w = _widgets[index];
    if (!is_checkbox(w.type)) return;
    if (!w.hwnd) {
      w.pending_check = (int)state;
      return;
    }
    SendMessageW(w.hwnd, BM_SETCHECK, (WPARAM)state, 0);
  }

  neui_check_state_t Session::widget_get_check(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return NEUI_CHECK_UNCHECKED;
    auto& w = _widgets[index];
    if (!is_checkbox(w.type) || !w.hwnd) return NEUI_CHECK_UNCHECKED;
    LRESULT state = SendMessageW(w.hwnd, BM_GETCHECK, 0, 0);
    if (state == BST_CHECKED)       return NEUI_CHECK_CHECKED;
    if (state == BST_INDETERMINATE) return NEUI_CHECK_INDETERMINATE;
    return NEUI_CHECK_UNCHECKED;
  }

  static void set_check(neui_session_t session, neui_widget_t widget, neui_check_state_t state)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) s->widget_set_check(widget, state);
  }

  static neui_check_state_t get_check(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    return s ? s->widget_get_check(widget) : NEUI_CHECK_UNCHECKED;
  }

  void* Session::widget_get_native_handle(neui_widget_t widget)
  {
    uint32_t index = WidgetToIndex(widget);
    if (!_widgets.exists(index)) return nullptr;
    return static_cast<void*>(_widgets[index].hwnd);
  }

  static void* get_native_handle(neui_session_t session, neui_widget_t widget)
  {
    auto s = get_session_for_widget(session, widget);
    if (s) return s->widget_get_native_handle(widget);
    return nullptr;
  }

  // Native Win32 controls carry WS_TABSTOP at creation and native
  // WM_GETDLGCODE / IsDialogMessage handles traversal, so this is a no-op.
  // The entry exists to keep the vtable layout in sync with neui_widget_api_t.
  static void set_tab_stop(neui_session_t /*session*/, neui_widget_t /*widget*/, bool /*enabled*/)
  {
  }

  static void set_owner(neui_session_t session, neui_widget_t dialog, neui_widget_t owner)
  {
    auto s = get_session_for_widget(session, dialog);
    if (!s) return;
    if (!widget_belongs_to_session(owner, s->session_id())) return;
    s->widget_set_owner(dialog, owner);
  }

  static void get_pos(neui_session_t session, neui_widget_t widget,
                      int* x, int* y)
  {
    auto s = get_session_for_widget(session, widget);
    if (!s) return;
    auto* wd = s->get_widget(widget.id);
    if (!wd) return;
    if (x) *x = wd->x;
    if (y) *y = wd->y;
  }

  static void get_size(neui_session_t session, neui_widget_t widget,
                       int* width, int* height)
  {
    auto s = get_session_for_widget(session, widget);
    if (!s) return;
    auto* wd = s->get_widget(widget.id);
    if (!wd) return;
    if (width)  *width  = wd->width;
    if (height) *height = wd->height;
  }

  // ---------------------------------------------------------------------------
  // Popup menu (context menu, free of the menubar)

  static int NEUI_ABI popup_menu(neui_session_t session, neui_widget_t anchor,
                                  int x, int y, const char* const* items)
  {
    auto* s = get_session(session);
    if (!s) return 0;
    auto* wd = s->get_widget(WidgetToIndex(anchor));
    if (!wd || !wd->session) return 0;
    if (!items) return 0;

    // Find the parent frame's HWND for screen-coord conversion + as the
    // owner of TrackPopupMenuEx.
    HWND owner = wd->session->find_parent_hwnd(wd->index);
    if (!owner) owner = wd->hwnd;
    if (!owner) return 0;

    HMENU h = CreatePopupMenu();
    if (!h) return 0;

    int count = 0;
    for (int i = 0; items[i] != nullptr; ++i) {
      const char* s8 = items[i];
      bool is_sep = (s8[0] == '-' && s8[1] == 0);
      ++count;
      if (is_sep) {
        AppendMenuW(h, MF_SEPARATOR, 0, nullptr);
      } else {
        // Convert UTF-8 to UTF-16.
        int wlen = MultiByteToWideChar(CP_UTF8, 0, s8, -1, nullptr, 0);
        std::wstring ws(wlen ? wlen - 1 : 0, L'\0');
        if (wlen > 1) {
          MultiByteToWideChar(CP_UTF8, 0, s8, -1, ws.data(), wlen);
        }
        AppendMenuW(h, MF_STRING, static_cast<UINT_PTR>(count), ws.c_str());
      }
    }

    // Convert (x, y) from anchor-local logical to screen physical.
    UINT dpi = wd->session->get_dpi_for_widget(wd->index);
    if (dpi == 0) dpi = 96;
    POINT pt;
    pt.x = LogicalToPhysical(wd->x + x, dpi);
    pt.y = LogicalToPhysical(wd->y + y, dpi);
    // Walk up to the frame: anchor's (x, y) above are relative to its
    // immediate parent. find_parent_hwnd returned the frame; we need the
    // anchor's parent HWND to map correctly. For knob widgets the parent
    // is the frame, so this works. For deeper nesting we'd need to walk.
    if (wd->hwnd) {
      // anchor itself has an HWND - pt(x,y) is in its client coords.
      pt.x = LogicalToPhysical(x, dpi);
      pt.y = LogicalToPhysical(y, dpi);
      ClientToScreen(wd->hwnd, &pt);
    } else {
      ClientToScreen(owner, &pt);
    }

    UINT picked = TrackPopupMenuEx(h,
        TPM_RETURNCMD | TPM_NONOTIFY | TPM_LEFTALIGN | TPM_TOPALIGN,
        pt.x, pt.y, owner, nullptr);
    DestroyMenu(h);
    return static_cast<int>(picked);
  }

  neui_widget_api_t widgets_api = {
    create,
    destroy,
    show,
    hide,
    set_pos,
    set_size,
    set_emit_events,
    set_text,
    get_text,
    get_first_child,
    get_next_sibling,
    set_focus,
    set_check,
    get_check,
    get_native_handle,
    set_tab_stop,
    set_owner,
    get_pos,
    get_size,
    popup_menu,
  };

  // -------------------------------------------------------------------------
  // Attribute API

  namespace {
    WidgetData* get_widget_ptr(neui_session_t session, neui_widget_t widget) {
      auto* s = get_session(session);
      if (!s) return nullptr;
      return s->get_widget(WidgetToIndex(widget));
    }
  }

  static int NEUI_ABI a_set_int(neui_session_t session, neui_widget_t widget,
                                 const char* key, int32_t value)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key) return 0;
    neui_detail::ensure_attrs(w->attrs).set_int(key, value);

    // Live re-application for paint-affecting int attrs on self-painted
    // widgets. NEUI_ATTR_BACKGROUND drives body fill (SECTION), the
    // begin_frame clear (KNOB / IMAGE), so a runtime change needs a
    // repaint. Region geometry doesn't depend on colour, so SECTION can
    // skip apply_section_region_w32 here.
    if (w->hwnd && w->type && !strcmp(key, NEUI_ATTR_BACKGROUND) &&
        (!strcmp(w->type, NEUI_W_SECTION) ||
         !strcmp(w->type, NEUI_W_KNOB)    ||
         !strcmp(w->type, NEUI_W_IMAGE))) {
      InvalidateRect(w->hwnd, nullptr, FALSE);
    }
    return 1;
  }

  static int32_t NEUI_ABI a_get_int(neui_session_t session, neui_widget_t widget,
                                     const char* key, int32_t default_value)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key || !w->attrs) return default_value;
    return w->attrs->get_int(key, default_value);
  }

  static int NEUI_ABI a_set_string(neui_session_t session, neui_widget_t widget,
                                    const char* key, const char* value)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key) return 0;
    neui_detail::ensure_attrs(w->attrs).set_string(key, value);

    // Live re-application for behavior-bearing keys.
    if (w->isroot && w->hwnd && !strcmp(key, NEUI_ATTR_ICON_PATH)) {
      neui_detail::apply_window_icon(w->hwnd, value, &w->native_icon);
    }
    // Section: align change moves the title chip, so the window region
    // has to be rebuilt before the next paint. Also covers background
    // changes via the InvalidateRect (region geometry doesn't depend on
    // colour, but a repaint is needed to pick up the new fill).
    if (w->hwnd && w->type && !strcmp(w->type, NEUI_W_SECTION) &&
        !strcmp(key, NEUI_ATTR_ALIGN_TEXT)) {
      apply_section_region_w32(*w);
      InvalidateRect(w->hwnd, nullptr, FALSE);
    }
    return 1;
  }

  static const char* NEUI_ABI a_get_string(neui_session_t session,
                                            neui_widget_t widget, const char* key)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key || !w->attrs) return nullptr;
    return w->attrs->get_string(key);
  }

  static int NEUI_ABI a_has(neui_session_t session, neui_widget_t widget,
                             const char* key)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key) return 0;
    return (w->attrs && w->attrs->has(key)) ? 1 : 0;
  }

  static int NEUI_ABI a_remove(neui_session_t session, neui_widget_t widget,
                                const char* key)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key || !w->attrs) return 0;
    return w->attrs->remove(key) ? 1 : 0;
  }

  static int NEUI_ABI a_set_float(neui_session_t session, neui_widget_t widget,
                                   const char* key, float value)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key) return 0;

    // Clamp + (when steps is set) snap NEUI_PARAM_VALUE on entry so
    // attribute storage and anything reading it back (paint, native control
    // state) stay in sync.
    float stored = value;
    if (!strcmp(key, NEUI_PARAM_VALUE)) {
      stored = clamp01_w32(value);
      stored = snap_to_steps_w32(stored, widget_get_steps_w32(*w));
    }
    neui_detail::ensure_attrs(w->attrs).set_float(key, stored);

    // Live re-application for value-bearing widgets so programmatic sets
    // immediately reflect in the visible state. Programmatic set is
    // intentionally silent - no NEUI_EVENT_VALUE_CHANGED.
    if (w->hwnd && w->type && !strcmp(key, NEUI_PARAM_VALUE)) {
      if (!strcmp(w->type, NEUI_W_SLIDER)) {
        DWORD style = static_cast<DWORD>(GetWindowLongW(w->hwnd, GWL_STYLE));
        bool vertical = (style & TBS_VERT) != 0;
        int  pos = static_cast<int>((vertical ? (1.0f - stored) : stored) * 1000.0f + 0.5f);
        SendMessageW(w->hwnd, TBM_SETPOS, TRUE, pos);
      } else if (!strcmp(w->type, NEUI_W_KNOB)) {
        InvalidateRect(w->hwnd, nullptr, FALSE);
      }
    }

    // NEUI_ATTR_ROTATION on IMAGE: invalidate so the bitmap repaints with
    // the new transform on the next paint pass. Native controls don't
    // need this; only painted widgets read the attr in their draw path.
    if (w->hwnd && w->type && !strcmp(key, NEUI_ATTR_ROTATION) &&
        !strcmp(w->type, NEUI_W_IMAGE)) {
      InvalidateRect(w->hwnd, nullptr, FALSE);
    }
    return 1;
  }

  static float NEUI_ABI a_get_float(neui_session_t session, neui_widget_t widget,
                                     const char* key, float default_value)
  {
    auto* w = get_widget_ptr(session, widget);
    if (!w || !key || !w->attrs) return default_value;
    return w->attrs->get_float(key, default_value);
  }

  static int NEUI_ABI a_set_session_int(neui_session_t session,
                                         const char* key, int32_t value)
  {
    auto* s = get_session(session);
    if (!s || !key) return 0;
    s->_session_attrs.set_int(key, value);
    // Live-apply the keys we know about. NEUI_ATTR_THEME_MODE is the
    // only session-level key with behaviour today; recompute the
    // effective palette and re-paint as if the system theme had flipped.
    if (!strcmp(key, NEUI_ATTR_THEME_MODE)) {
      s->on_theme_changed();
    }
    return 1;
  }

  static int32_t NEUI_ABI a_get_session_int(neui_session_t session,
                                             const char* key,
                                             int32_t default_value)
  {
    auto* s = get_session(session);
    if (!s || !key) return default_value;
    return s->_session_attrs.get_int(key, default_value);
  }

  neui_attr_api_t attrs_api = {
    NEUI_VERSION,
    a_set_int,
    a_get_int,
    a_set_string,
    a_get_string,
    a_has,
    a_remove,
    a_set_float,
    a_get_float,
    a_set_session_int,
    a_get_session_int,
  };

  // -------------------------------------------------------------------------
  // Clipboard API

  static int NEUI_ABI cb_set_text(neui_session_t /*session*/, const char* utf8)
  {
    if (!utf8) return 0;
    return neui_detail::clipboard_set_text_win32(
             utf8, static_cast<uint32_t>(strlen(utf8))) ? 1 : 0;
  }

  static int NEUI_ABI cb_get_text(neui_session_t /*session*/, char* buf, int buflen)
  {
    return neui_detail::clipboard_get_text_win32(buf, buflen);
  }

  static bool NEUI_ABI cb_has_text(neui_session_t /*session*/)
  {
    return neui_detail::clipboard_has_text_win32();
  }

  static neui_clipboard_item_t NEUI_ABI cb_read(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return neui_clipboard_item_none;
    if (!neui_detail::clipboard_has_text_win32())
      return neui_clipboard_item_none;
    int n = neui_detail::clipboard_get_text_win32(nullptr, 0);
    if (n <= 0) return neui_clipboard_item_none;
    std::vector<char> buf(static_cast<size_t>(n));
    if (neui_detail::clipboard_get_text_win32(buf.data(), n) <= 0)
      return neui_clipboard_item_none;
    uint32_t id = s->_clipboard_items.allocate();
    auto* item = s->_clipboard_items.get(id);
    item->set_format(NEUI_CLIPBOARD_MIME_TEXT, buf.data(),
                     static_cast<uint32_t>(n));
    return { id };
  }

  static neui_clipboard_item_t NEUI_ABI cb_create_item(neui_session_t session)
  {
    auto* s = get_session(session);
    if (!s) return neui_clipboard_item_none;
    return { s->_clipboard_items.allocate() };
  }

  static void NEUI_ABI cb_release(neui_session_t session,
                                   neui_clipboard_item_t item)
  {
    auto* s = get_session(session);
    if (!s) return;
    s->_clipboard_items.release(item.id);
  }

  static int NEUI_ABI cb_write(neui_session_t session,
                                neui_clipboard_item_t item)
  {
    auto* s = get_session(session);
    if (!s) return 0;
    auto* it = s->_clipboard_items.get(item.id);
    if (!it) return 0;
    if (!it->has_format(NEUI_CLIPBOARD_MIME_TEXT)) return 0;
    int n = it->get_format(NEUI_CLIPBOARD_MIME_TEXT, nullptr, 0);
    if (n <= 0) return 0;
    std::vector<char> buf(static_cast<size_t>(n));
    it->get_format(NEUI_CLIPBOARD_MIME_TEXT, buf.data(), n);
    uint32_t length = static_cast<uint32_t>(n);
    if (length > 0 && buf[length - 1] == '\0') length -= 1;
    return neui_detail::clipboard_set_text_win32(buf.data(), length) ? 1 : 0;
  }

  static int NEUI_ABI cb_item_set_format(neui_session_t session,
                                          neui_clipboard_item_t item,
                                          const char* mime,
                                          const void* data, uint32_t length)
  {
    auto* s = get_session(session);
    if (!s || !mime) return 0;
    if (strcmp(mime, NEUI_CLIPBOARD_MIME_TEXT) != 0) return 0;
    auto* it = s->_clipboard_items.get(item.id);
    if (!it) return 0;
    it->set_format(mime, data, length);
    return 1;
  }

  static int NEUI_ABI cb_item_get_format(neui_session_t session,
                                          neui_clipboard_item_t item,
                                          const char* mime,
                                          void* buf, int buflen)
  {
    auto* s = get_session(session);
    if (!s || !mime) return -1;
    auto* it = s->_clipboard_items.get(item.id);
    if (!it) return -1;
    return it->get_format(mime, buf, buflen);
  }

  static bool NEUI_ABI cb_item_has_format(neui_session_t session,
                                           neui_clipboard_item_t item,
                                           const char* mime)
  {
    auto* s = get_session(session);
    if (!s || !mime) return false;
    auto* it = s->_clipboard_items.get(item.id);
    return it && it->has_format(mime);
  }

  neui_clipboard_api_t clipboard_api = {
    NEUI_VERSION,
    cb_set_text,
    cb_get_text,
    cb_has_text,
    cb_read,
    cb_create_item,
    cb_release,
    cb_write,
    cb_item_set_format,
    cb_item_get_format,
    cb_item_has_format,
  };
}
