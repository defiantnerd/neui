#define NOMINMAX
#include "window.h"
#include <commctrl.h>
#include <dwmapi.h>
#include <uxtheme.h>
#include <shellapi.h>   // CommandLineToArgvW (was transitively pulled in
                         // before WIN32_LEAN_AND_MEAN started excluding it
                         // via the asset_manager_w32.h include chain).
#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")
#include "../../backends/d2d/d2d_backend.h"
#include "../shared/theme_palette.h"
#include "../shared/widget_paint_section.h"
#include "../shared/win32/theme_brushes_win32.h"
#include "../shared/win32/theme_provider_win32.h"
#include "../shared/win32/dark_menu_win32.h"
#include "../shared/win32/dark_menubar_win32.h"

#include <winrt/windows.foundation.h>
#include <winrt/Windows.UI.ViewManagement.h>

int main(int argc, wchar_t** argv);

namespace win32_host
{
  // Defined in widgets.cpp; rebuilds the section's window region from its
  // current size + text + alignment. Called here on WM_SIZE so a resize
  // (or DPI change, which propagates through WM_SIZE) refreshes the
  // region in physical pixels.
  void apply_section_region_w32(WidgetData& wd);
  void section_apply_layout_changes_w32(WidgetData& wd);
  // Defined in widgets.cpp. Re-flows a TABVIEW's chip strip + re-sizes the
  // selected page to the content body rect. Called here on WM_SIZE.
  void tabview_relayout_w32(WidgetData& wd);
}

// DWMWA_USE_IMMERSIVE_DARK_MODE attribute id varies by Windows version.
// Win11 / late-Win10: 20. Early Win10 1809-1909: 19. We try 20 first and
// fall back to 19 if the call fails.
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE
#define DWMWA_USE_IMMERSIVE_DARK_MODE 20
#endif
#ifndef DWMWA_USE_IMMERSIVE_DARK_MODE_OLD
#define DWMWA_USE_IMMERSIVE_DARK_MODE_OLD 19
#endif

static HINSTANCE g_hInstance = nullptr;
static int g_appwindow_count = 0;

namespace win32_host
{

  extern std::vector<std::unique_ptr<Session>> sessions;

  // Defined in widgets.cpp. Declared here so ChildSubclassProc can reset
  // a slider's value on double-click.
  void widget_reset_to_default_w32(WidgetData& wd);

  // Defined in widgets.cpp. Declared here so PaintedWndProc can repaint
  // a CUSTOMDRAW widget when hover / press transitions affect its compound
  // layer state filters (NEUI_LAYER_STATE_*).
  void w32_invalidate_if_state_filtered_compound(WidgetData& wd);

  // Shared native-control parent-notification routing (defined below). Any
  // wndproc that can be a native control's parent calls these so the control
  // reports its events regardless of which HWND owns it: the frame
  // (AppWindowProc), a scrolling SECTION's body (SectionBodyWndProc), or a
  // non-scrolling SECTION / TABPAGE painted HWND (PaintedWndProc).
  bool    route_native_command_notification(Session* sess, WPARAM wParam, LPARAM lParam);
  bool    route_native_scroll_notification(Session* sess, UINT msg, WPARAM wParam, LPARAM lParam);
  LRESULT route_native_notify(Session* sess, LPARAM lParam, bool& handled);

  // Re-apply system-theme state to a frame. Called from
  // Session::on_theme_changed for every frame in the session;
  // gated internally on NEUI_ATTR_FOLLOW_SYSTEM_THEME so frames that
  // didn't opt in are left at the OS default.
  void apply_theme_to_frame_w32(WidgetData& wd)
  {
    if (!wd.hwnd) return;
    bool follow = wd.attrs &&
                  wd.attrs->get_int(NEUI_ATTR_FOLLOW_SYSTEM_THEME, 0) != 0;
    // Scope the override to this session before reading is_dark - the
    // function is called from frame creation and from on_theme_changed,
    // and in multi-session apps a sibling session's last write could
    // otherwise leak in.
    neui_detail::ScopedPaletteOverride scope(
      wd.session ? wd.session->effective_palette_ptr() : nullptr);
    bool want_dark = follow && neui_detail::current_palette().is_dark;
    BOOL dark = want_dark ? TRUE : FALSE;
    HRESULT hr = DwmSetWindowAttribute(wd.hwnd,
                                       DWMWA_USE_IMMERSIVE_DARK_MODE,
                                       &dark, sizeof(dark));
    if (FAILED(hr)) {
      // Pre-Win11 / early-Win10 1809-1909 use attribute id 19.
      DwmSetWindowAttribute(wd.hwnd,
                            DWMWA_USE_IMMERSIVE_DARK_MODE_OLD,
                            &dark, sizeof(dark));
    }
    // Native HMENU dark mode: opt this window in (uxtheme private API).
    // Only when the frame opted into theme tracking - otherwise leave
    // the menu at OS default to preserve backwards-compat behaviour.
    if (follow) neui_detail::apply_dark_window_mode(wd.hwnd, want_dark);
  }

  // Helper: lookup the WidgetData* for a child HWND by its dialog ID
  // (we set it as the HMENU param on CreateWindowExW). Returns nullptr
  // if not a recognised child.
  static WidgetData* widget_for_child_hwnd_w32(WidgetData* parent_wd, HWND child)
  {
    if (!parent_wd || !parent_wd->session || !child) return nullptr;
    UINT id = (UINT)GetDlgCtrlID(child);
    return parent_wd->session->get_widget(id);
  }

  void apply_native_theme_w32(WidgetData& wd)
  {
    if (!wd.hwnd || !wd.session) return;
    // Skip frames - their title bar follows DWM dark mode (handled by
    // apply_theme_to_frame_w32) and SetWindowTheme(DarkMode_Explorer)
    // on a top-level app frame doesn't do anything useful.
    if (wd.type && (!strcmp(wd.type, NEUI_W_APPWINDOW)
                 || !strcmp(wd.type, NEUI_W_PLUGWINDOW)
                 || !strcmp(wd.type, NEUI_W_DIALOG))) return;
    bool follow = wd.session->frame_follows_theme(&wd);
    // Scope the palette override to this widget's session before reading
    // is_dark (multi-session correctness).
    neui_detail::ScopedPaletteOverride scope(wd.session->effective_palette_ptr());
    bool is_dark = follow && neui_detail::current_palette().is_dark;

    // Pick the theme class. DarkMode_Explorer drives dark scrollbars,
    // dark treeview/listview chrome, dark button face, dark combobox
    // dropdown. Explorer (or empty) restores the light look. When the
    // frame isn't opted in, leave the OS default by passing null sub-app
    // - equivalent to "let whatever the manifest enabled stand".
    if (follow) {
      const wchar_t* theme = is_dark ? L"DarkMode_Explorer" : L"Explorer";
      SetWindowTheme(wd.hwnd, theme, nullptr);
    } else {
      // Restore the default visual style chain.
      SetWindowTheme(wd.hwnd, nullptr, nullptr);
    }

    // TreeView body bg / text / line colours aren't controlled by
    // SetWindowTheme - they're per-control properties. NM_CUSTOMDRAW
    // already handles per-item text + bg, but the area below the last
    // item and the +/- chevron lines use these.
    if (wd.type && !strcmp(wd.type, NEUI_W_TREEVIEW)) {
      using neui_detail::ColorRole;
      if (follow) {
        TreeView_SetBkColor(wd.hwnd,
          neui_detail::colorref_from_argb(neui_detail::color(ColorRole::control_bg)));
        TreeView_SetTextColor(wd.hwnd,
          neui_detail::colorref_from_argb(neui_detail::color(ColorRole::text_primary)));
        TreeView_SetLineColor(wd.hwnd,
          neui_detail::colorref_from_argb(neui_detail::color(ColorRole::text_secondary)));
      } else {
        // -1 means "use system defaults".
        TreeView_SetBkColor(wd.hwnd, (COLORREF)-1);
        TreeView_SetTextColor(wd.hwnd, (COLORREF)-1);
        TreeView_SetLineColor(wd.hwnd, (COLORREF)CLR_DEFAULT);
      }
    }

    // ComboBox is a composite control: the outer HWND hosts a static /
    // edit subwindow showing the current selection, and a separate
    // dropdown listbox HWND. SetWindowTheme on the outer doesn't always
    // propagate, so theme the subwindows explicitly. The "CFD" theme
    // class ("Combo Filter Drop") is what File Explorer's address bar /
    // Settings dialogs use for the combobox edit area; "Explorer" is
    // right for the dropdown list. Use the DarkMode_ prefixed variants
    // in dark mode, plain in light, restore-default when not following.
    if (wd.type && !strcmp(wd.type, NEUI_W_COMBOBOX)) {
      const wchar_t* outer_theme = follow ? (is_dark ? L"DarkMode_CFD"      : L"CFD")      : nullptr;
      const wchar_t* item_theme  = follow ? (is_dark ? L"DarkMode_CFD"      : L"CFD")      : nullptr;
      const wchar_t* list_theme  = follow ? (is_dark ? L"DarkMode_Explorer" : L"Explorer") : nullptr;
      // Outer HWND override (the earlier SetWindowTheme used the generic
      // "Explorer" class - comboboxes need CFD for their edit chrome).
      SetWindowTheme(wd.hwnd, outer_theme, nullptr);
      COMBOBOXINFO cbi = {};
      cbi.cbSize = sizeof(cbi);
      if (GetComboBoxInfo(wd.hwnd, &cbi)) {
        if (cbi.hwndItem) SetWindowTheme(cbi.hwndItem, item_theme, nullptr);
        if (cbi.hwndList) SetWindowTheme(cbi.hwndList, list_theme, nullptr);
      }
    }
  }

  void set_hinstance(HINSTANCE h) { g_hInstance = h; }
  HINSTANCE get_hinstance() { return g_hInstance; }

  void register_classes()
  {
    INITCOMMONCONTROLSEX icx = {};
    icx.dwSize = sizeof(icx);
    icx.dwICC = ICC_STANDARD_CLASSES | ICC_WIN95_CLASSES;
    InitCommonControlsEx(&icx);

    WNDCLASSEXW wc = {};
    wc.cbSize        = sizeof(wc);
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = AppWindowProc;
    wc.hInstance     = g_hInstance;
    wc.hCursor       = LoadCursorW(nullptr, (LPCWSTR)IDC_ARROW);  // resource ID, not a string pointer
    wc.hbrBackground = (HBRUSH)(COLOR_WINDOW + 1);

    wc.lpszClassName = L"neui.appwindow";
    RegisterClassExW(&wc);

    wc.lpszClassName = L"neui.plugwindow";
    RegisterClassExW(&wc);

    wc.lpszClassName = L"neui.dialog";
    RegisterClassExW(&wc);

    // Shared class for self-painted widgets (KNOB, SECTION, IMAGE,
    // CUSTOMDRAW). Hosts a per-widget D2D context; paint logic plugs in
    // via WidgetData::paint_fn.
    wc.style         = CS_HREDRAW | CS_VREDRAW | CS_DBLCLKS;
    wc.lpfnWndProc   = PaintedWndProc;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"neui.painted";
    RegisterClassExW(&wc);

    // Scrolling-SECTION inner body container. Hosts the section's child
    // widgets so Win32's default subview-clip naturally hides children
    // that overflow the body rect on either axis - no per-child window
    // regions needed. Paint just fills the body bg colour (read from the
    // parent section's NEUI_ATTR_BACKGROUND attr at draw time). WS_CLIPCHILDREN
    // is set on each instance so the bg fill paints around children.
    wc.style         = CS_HREDRAW | CS_VREDRAW;
    wc.lpfnWndProc   = SectionBodyWndProc;
    wc.hbrBackground = nullptr;
    wc.lpszClassName = L"neui.sectionbody";
    RegisterClassExW(&wc);
  }

  // Shared WndProc for self-painted widgets (NEUI_W_KNOB and future
  // painted controls). Owns the per-widget D2D context lifecycle and
  // dispatches paint via WidgetData::paint_fn. Mouse / wheel / button
  // events route through painted_msg_fn for widget-specific behaviour
  // (e.g. knob drag-to-value); dispatch to the client happens in the
  // installed ChildSubclassProc as for any other interactive widget.
  LRESULT CALLBACK PaintedWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
  {
    WidgetData* wd = reinterpret_cast<WidgetData*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
    auto* backend  = neui_d2d_backend::get_backend();

    switch (msg) {
    case WM_NCCREATE: {
      CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
      wd = reinterpret_cast<WidgetData*>(cs->lpCreateParams);
      if (wd) SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(wd));
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_CREATE: {
      if (wd && backend) {
        RECT rc; GetClientRect(hwnd, &rc);
        wd->paint_ctx = backend->create_context(hwnd,
                                                 static_cast<uint32_t>(rc.right),
                                                 static_cast<uint32_t>(rc.bottom));
        // Stash the HWND's DPI so create_child_windows can scale this
        // widget's own children (relevant for container painted widgets
        // like NEUI_W_SECTION). Without this, parent_wd.dpi stays at the
        // default 96 and inner children are created with logical coords
        // treated as physical px.
        wd->dpi = GetDpiForWindow(hwnd);
      }
      return 0;
    }
    case WM_SIZE: {
      if (wd && backend && wd->paint_ctx) {
        backend->resize(wd->paint_ctx,
                        static_cast<uint32_t>(LOWORD(lParam)),
                        static_cast<uint32_t>(HIWORD(lParam)));
      }
      // SECTION region tracks physical-pixel widget size, so a resize
      // must rebuild it before the next paint. Scrolling sections also
      // need their layout cache rebuilt + children repositioned (the
      // scroll position may now be out of range for the smaller body).
      if (wd && wd->type &&
          (!strcmp(wd->type, NEUI_W_SECTION) ||
           !strcmp(wd->type, NEUI_W_TABPAGE))) {
        apply_section_region_w32(*wd);
        // Scrolling sections / pages also need their layout cache rebuilt +
        // children repositioned (the scroll position may now be out of
        // range for the smaller body, and clamp runs in the layout
        // helper - children must reflect the new scroll before the next
        // mouse event).
        if (wd->section_scroll_state)
          section_apply_layout_changes_w32(*wd);
      }
      // TABVIEW resize: re-flow the chip strip + re-size the selected page
      // to the new content body rect.
      if (wd && wd->type && !strcmp(wd->type, NEUI_W_TABVIEW))
        tabview_relayout_w32(*wd);
      return 0;
    }
    case WM_ERASEBKGND:
      return 1;  // background handled by paint_fn
    case WM_PAINT: {
      PAINTSTRUCT ps;
      BeginPaint(hwnd, &ps);
      EndPaint(hwnd, &ps);
      if (wd && backend && wd->paint_ctx && wd->paint_fn) {
        // Fire WIDGET_PREUPDATE so the client can refresh attribute-driven
        // state (e.g. NEUI_PARAM_VALUE) before paint_fn reads it.
        if (wd->emit_events && wd->session) {
          neui_widget_t wid = { wd->widget_id };
          neui_event_t ev{};
          ev.type = NEUI_EVENT_WIDGET_PREUPDATE;
          ev.data.preupdate.widget = wid;
          wd->session->dispatch_event(&ev);
        }

        RECT rc; GetClientRect(hwnd, &rc);
        // Resolve the actual DPI from the parent frame (wd->dpi defaults to 96
        // on child widgets and isn't updated, so trusting it would silently
        // break HiDPI rendering - coords would be treated as logical when
        // they're really physical).
        UINT dpi = wd->session ? wd->session->get_dpi_for_widget(wd->index) : 96;
        if (dpi == 0) dpi = 96;
        float lw = static_cast<float>(rc.right)  * 96.0f / static_cast<float>(dpi);
        float lh = static_cast<float>(rc.bottom) * 96.0f / static_cast<float>(dpi);
        bool focused = (GetFocus() == hwnd);

        // Background colour: transparent for an OVERLAY CUSTOMDRAW (its
        // composition target composites over the siblings beneath, so
        // unpainted pixels must stay clear) -> client override -> theme
        // palette (when the owning frame opts in) -> system COLOR_WINDOW.
        uint32_t clear_argb;
        if (wd->attrs && wd->attrs->get_int(NEUI_ATTR_OVERLAY, 0) != 0) {
          clear_argb = 0x00000000u;  // fully transparent (premultiplied)
        } else if (wd->attrs && wd->attrs->has(NEUI_ATTR_BACKGROUND)) {
          clear_argb = static_cast<uint32_t>(
            wd->attrs->get_int(NEUI_ATTR_BACKGROUND, 0));
        } else if (wd->session && wd->session->frame_follows_theme(wd)) {
          clear_argb = neui_detail::color(neui_detail::ColorRole::panel_bg);
        } else {
          COLORREF bg = GetSysColor(COLOR_WINDOW);
          clear_argb = 0xFF000000u
                     | (static_cast<uint32_t>(GetRValue(bg)) << 16)
                     | (static_cast<uint32_t>(GetGValue(bg)) <<  8)
                     |  static_cast<uint32_t>(GetBValue(bg));
        }
        backend->begin_frame(wd->paint_ctx, clear_argb);
        wd->paint_fn(backend, wd->paint_ctx, lw, lh, *wd, focused);
        backend->end_frame(wd->paint_ctx);
      }
      return 0;
    }
    case WM_LBUTTONDOWN:
      SetCapture(hwnd);
      if (wd && !wd->pressed) {
        wd->pressed = true;
        w32_invalidate_if_state_filtered_compound(*wd);
      }
      if (wd && wd->painted_msg_fn) wd->painted_msg_fn(*wd, msg, wParam, lParam);
      return 0;
    case WM_LBUTTONUP:
      if (GetCapture() == hwnd) ReleaseCapture();
      if (wd && wd->pressed) {
        wd->pressed = false;
        w32_invalidate_if_state_filtered_compound(*wd);
      }
      if (wd && wd->painted_msg_fn) wd->painted_msg_fn(*wd, msg, wParam, lParam);
      return 0;
    case WM_MOUSEMOVE:
      if (wd && !wd->hovered) {
        TRACKMOUSEEVENT tme = {};
        tme.cbSize    = sizeof(tme);
        tme.dwFlags   = TME_LEAVE;
        tme.hwndTrack = hwnd;
        TrackMouseEvent(&tme);
        wd->hovered = true;
        w32_invalidate_if_state_filtered_compound(*wd);
      }
      if (wd && wd->painted_msg_fn) wd->painted_msg_fn(*wd, msg, wParam, lParam);
      return 0;
    case WM_MOUSELEAVE:
      if (wd && wd->hovered) {
        wd->hovered = false;
        w32_invalidate_if_state_filtered_compound(*wd);
      }
      if (wd && wd->painted_msg_fn) wd->painted_msg_fn(*wd, msg, wParam, lParam);
      return 0;
    case WM_LBUTTONDBLCLK:
    case WM_MOUSEWHEEL:
    case WM_MOUSEHWHEEL:
    case WM_RBUTTONDOWN:
    case WM_RBUTTONUP:
      if (wd && wd->painted_msg_fn) wd->painted_msg_fn(*wd, msg, wParam, lParam);
      return 0;
    case WM_KEYDOWN:
      // Forward to the widget's message hook (e.g. knob's arrow / Home /
      // End handling). The ChildSubclassProc still fires NEUI_EVENT_KEYDOWN
      // to the client first; this runs after.
      if (wd && wd->painted_msg_fn) wd->painted_msg_fn(*wd, msg, wParam, lParam);
      return 0;
    case WM_CHAR:
      // Painted-widget character input (GRID cell editor today). Routed so
      // the GRID can stuff typed codepoints into its edit buffer without
      // needing a focus-bound native edit control.
      if (wd && wd->painted_msg_fn) wd->painted_msg_fn(*wd, msg, wParam, lParam);
      return 0;
    case WM_KILLFOCUS:
      // Forward focus loss so the GRID can commit / cancel an open in-place
      // cell editor. Without this, Tab-traversal would leave the editor
      // open over a widget that no longer has the keyboard.
      if (wd && wd->painted_msg_fn) wd->painted_msg_fn(*wd, msg, wParam, lParam);
      return 0;
    case WM_TIMER:
      // Grid uses this to drive the 60 Hz spring-back animation when smooth
      // scroll is active. Forward so the widget's painted_msg_fn can step
      // the animation and decide whether to kill the timer.
      if (wd && wd->painted_msg_fn) wd->painted_msg_fn(*wd, msg, wParam, lParam);
      return 0;
    case WM_GETDLGCODE: {
      // Claim arrow keys so the dialog manager (IsDialogMessage in the
      // message pump) doesn't reinterpret them as focus navigation. Tab /
      // Shift+Tab stay with the dialog manager - we don't set DLGC_WANTTAB.
      //
      // When IsDialogMessage is probing a specific key (wParam = the VK
      // code, lParam = the MSG*), claim VK_RETURN and VK_ESCAPE for the
      // GRID widget so the in-place cell editor can use Enter to commit /
      // open and Escape to cancel. Without this, IsDialogMessage swallows
      // Enter (looking for a default button) and the keypress never reaches
      // the grid's WndProc.
      if (wParam != 0 && wd && wd->type && !strcmp(wd->type, NEUI_W_GRID)) {
        if (wParam == VK_RETURN || wParam == VK_ESCAPE)
          return DLGC_WANTMESSAGE;
      }
      // CUSTOMDRAW clients dispatch keys via NEUI_EVENT_KEYDOWN, so they
      // also want unmodified Enter / Escape to land in the client.
      if (wParam != 0 && wd && wd->type && !strcmp(wd->type, NEUI_W_CUSTOMDRAW)) {
        if (wParam == VK_RETURN || wParam == VK_ESCAPE)
          return DLGC_WANTMESSAGE;
      }
      // GRID with an open in-place cell editor wants every character
      // (DLGC_WANTCHARS keeps the dialog manager from interpreting them as
      // mnemonics). CUSTOMDRAW gets the same treatment so client text
      // input doesn't get filtered. Both already claim arrows above.
      LRESULT code = DLGC_WANTARROWS;
      if (wd && wd->type && wd->grid_model && wd->grid_model->edit.active &&
          !strcmp(wd->type, NEUI_W_GRID)) {
        code |= DLGC_WANTCHARS;
      }
      if (wd && wd->type && !strcmp(wd->type, NEUI_W_CUSTOMDRAW)) {
        code |= DLGC_WANTCHARS;
      }
      return code;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      // STATIC controls (labels, checkbox/radio text) parented to a
      // SECTION HWND ask their parent for a background brush. The frame's
      // AppWindowProc handles this for direct frame children; we mirror
      // that here so labels inside a section match the section's body
      // fill instead of falling back to DefWindowProc's system default
      // (which doesn't track the theme palette).
      if (wd && wd->session && wd->type &&
          (!strcmp(wd->type, NEUI_W_SECTION) ||
           !strcmp(wd->type, NEUI_W_TABPAGE))) {
        using neui_detail::ColorRole;
        neui_detail::ScopedPaletteOverride scope(
          wd->session->effective_palette_ptr());

        // Resolve the section's body colour the same way paint_section_w32
        // does so the static's bg lines up exactly.
        uint32_t bg_argb;
        if (wd->attrs && wd->attrs->has(NEUI_ATTR_BACKGROUND)) {
          bg_argb = static_cast<uint32_t>(
            wd->attrs->get_int(NEUI_ATTR_BACKGROUND, 0));
        } else {
          bg_argb = neui_detail::shade(
            neui_detail::color(ColorRole::frame_bg),
            neui_detail::SECTION_BG_LIFT);
        }

        if (!wd->section_ctl_bg_brush ||
            wd->section_ctl_bg_brush_argb != bg_argb) {
          if (wd->section_ctl_bg_brush) DeleteObject(wd->section_ctl_bg_brush);
          wd->section_ctl_bg_brush      = neui_detail::brush_from_argb(bg_argb);
          wd->section_ctl_bg_brush_argb = bg_argb;
        }

        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, neui_detail::colorref_from_argb(
                            neui_detail::color(ColorRole::text_primary)));
        return reinterpret_cast<LRESULT>(wd->section_ctl_bg_brush);
      }
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_DESTROY: {
      // Give the grid (or any other painted widget) a chance to clean up
      // timers / transient state before the HWND goes away.
      if (wd && wd->painted_msg_fn) wd->painted_msg_fn(*wd, msg, wParam, lParam);
      if (wd && backend && wd->paint_ctx) {
        // Drop any asset-manager GPU bitmaps keyed on this ctx before
        // tearing it down (CUSTOMDRAW assets create per-ctx uploads on
        // first draw; without this, the cache would hold dangling
        // bitmap pointers for the rest of the session).
        if (wd->session)
          wd->session->_asset_manager.release_context(wd->paint_ctx, backend);
        backend->destroy_context(wd->paint_ctx);
        wd->paint_ctx = nullptr;
      }
      return 0;
    }
    case WM_NCHITTEST:
      // NEUI_ATTR_INPUT_TRANSPARENT: returning HTTRANSPARENT makes the OS
      // re-send the hit to the window beneath this one in z-order (the
      // sibling backdrop), so mouse messages fall through. WS_EX_TRANSPARENT
      // alone does NOT do this for a non-layered child window - it only
      // affects paint order - so the explicit HTTRANSPARENT is required for
      // a transparent overlay to be click-through.
      if (wd && wd->attrs &&
          wd->attrs->get_int(NEUI_ATTR_INPUT_TRANSPARENT, 0) != 0)
        return HTTRANSPARENT;
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    // A SECTION / TABPAGE is itself a painted widget, and its native-control
    // children (when the section isn't scrolling, so there's no body HWND)
    // parent directly to THIS painted HWND. Route their parent notifications
    // exactly as the frame / section-body procs do, otherwise a slider /
    // button / list inside a plain section or a tab page is silent. Shared
    // helpers keyed on the owning Session.
    case WM_HSCROLL:
    case WM_VSCROLL:
      if (wd && wd->session &&
          route_native_scroll_notification(wd->session, msg, wParam, lParam))
        return 0;
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_COMMAND:
      if (wd && wd->session &&
          route_native_command_notification(wd->session, wParam, lParam))
        return 0;
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    case WM_NOTIFY: {
      bool handled = false;
      LRESULT r = route_native_notify(wd ? wd->session : nullptr, lParam, handled);
      if (handled) return r;
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
  }

  // ---------------------------------------------------------------------
  // Native-control notification routing, shared by every wndproc that can
  // parent a Win32 common control. A control reports user changes to its
  // PARENT HWND (trackbar -> WM_HSCROLL/WM_VSCROLL, button/list/combo ->
  // WM_COMMAND, treeview -> WM_NOTIFY). The parent of a control inside a
  // scrolling SECTION is the body HWND (SectionBodyWndProc), not the frame,
  // so the body proc must run the same routing as AppWindowProc - otherwise
  // those controls are mute (no VALUE_CHANGED / CLICK / selection event).
  // Keyed on the owning Session (recovered from whichever WidgetData the
  // parent HWND carries) rather than a specific frame.
  // ---------------------------------------------------------------------

  // WM_COMMAND child-control notification (lParam = control HWND, non-zero).
  // Returns true when consumed; false for menu picks / accelerators
  // (lParam == 0), which only the frame proc handles.
  bool route_native_command_notification(Session* sess, WPARAM wParam, LPARAM lParam)
  {
    if (!sess || lParam == 0) return false;
    uint32_t   child_index  = static_cast<uint32_t>(LOWORD(wParam));
    WORD       notification = HIWORD(wParam);
    WidgetData* child       = sess->get_widget(child_index);
    if (child && child->emit_events) {
      if (notification == BN_CLICKED) {
        neui_widget_t wid = { child->widget_id };
        bool is_cb = child->type &&
                     (!strcmp(child->type, NEUI_W_CHECKBOX) || !strcmp(child->type, NEUI_W_CHECKBOX3));
        if (is_cb) {
          LRESULT bst = SendMessageW(reinterpret_cast<HWND>(lParam), BM_GETCHECK, 0, 0);
          neui_check_state_t state = (bst == BST_CHECKED)       ? NEUI_CHECK_CHECKED
                                   : (bst == BST_INDETERMINATE) ? NEUI_CHECK_INDETERMINATE
                                                                 : NEUI_CHECK_UNCHECKED;
          neui_event_t event = { NEUI_EVENT_CHECKBOX_CHANGED };
          event.data.checkbox = { wid, state };
          sess->dispatch_event(&event);
        } else {
          neui_event_t event = { NEUI_EVENT_MOUSE_BUTTON_CLICK };
          event.data.mouse = { wid, 0, 0, 0 };
          sess->dispatch_event(&event);
        }
      } else if (notification == LBN_SELCHANGE) {  // LBN_SELCHANGE == CBN_SELCHANGE == 1
        bool is_list  = child->type && !strcmp(child->type, NEUI_W_LISTBOX);
        bool is_combo = child->type && !strcmp(child->type, NEUI_W_COMBOBOX);
        if (is_list || is_combo) {
          UINT getsel = is_list ? LB_GETCURSEL : CB_GETCURSEL;
          LRESULT sel = SendMessageW(reinterpret_cast<HWND>(lParam), getsel, 0, 0);
          uint32_t sel_idx = (sel < 0) ? NEUI_ITEM_NONE : static_cast<uint32_t>(sel);
          neui_widget_t wid = { child->widget_id };
          neui_event_t event = { NEUI_EVENT_ITEM_SELECTED };
          event.data.item = { wid, sel_idx };
          sess->dispatch_event(&event);
        }
      }
    }
    return true;
  }

  // Trackbar (SLIDER) WM_HSCROLL/WM_VSCROLL -> VALUE_CHANGED. lParam is the
  // trackbar HWND (null for native scrollbars - ignored). Returns true when
  // the message belonged to a SLIDER child and was consumed.
  bool route_native_scroll_notification(Session* sess, UINT msg, WPARAM wParam, LPARAM lParam)
  {
    HWND ctrl = reinterpret_cast<HWND>(lParam);
    if (!ctrl || !sess) return false;

    // GetDlgCtrlID(ctrl) recovers wd.index (we pass it as the HMENU param
    // to CreateWindowExW).
    UINT        child_index = GetDlgCtrlID(ctrl);
    WidgetData* child       = sess->get_widget(child_index);
    if (!child || !child->type || strcmp(child->type, NEUI_W_SLIDER) != 0)
      return false;

    // Read the (possibly already-updated) thumb position. For most
    // notification codes Windows has updated TBM_GETPOS for us before
    // sending; the codes that don't update first are TB_THUMBPOSITION
    // and TB_THUMBTRACK, where HIWORD(wParam) carries the new value.
    WORD code = LOWORD(wParam);
    LRESULT pos;
    if (code == TB_THUMBPOSITION || code == TB_THUMBTRACK)
      pos = HIWORD(wParam);
    else
      pos = SendMessageW(ctrl, TBM_GETPOS, 0, 0);

    if (pos < 0)    pos = 0;
    if (pos > 1000) pos = 1000;
    float v = static_cast<float>(pos) / 1000.0f;

    // Vertical trackbars in Win32 send 0 at the top by default, but the
    // logical "fader" convention is 1.0 at the top - match that here so
    // the same NEUI_PARAM_VALUE means the same thing on both axes.
    if (msg == WM_VSCROLL) v = 1.0f - v;

    // Snap to discrete steps if NEUI_ATTR_STEPS is set, then push the
    // snapped position back to the trackbar so the thumb visually lands
    // on the tick (TBM_SETTICFREQ doesn't enforce snap by itself).
    int steps = child->attrs ? child->attrs->get_int(NEUI_ATTR_STEPS, 0) : 0;
    if (steps >= 2) {
      int sidx = static_cast<int>(v * static_cast<float>(steps - 1) + 0.5f);
      if (sidx < 0) sidx = 0;
      if (sidx >= steps) sidx = steps - 1;
      v = static_cast<float>(sidx) / static_cast<float>(steps - 1);
      int snap_pos = static_cast<int>((msg == WM_VSCROLL ? (1.0f - v) : v) * 1000.0f + 0.5f);
      if (snap_pos != pos)
        SendMessageW(ctrl, TBM_SETPOS, TRUE, snap_pos);
    }

    // Update the attribute (silent) so reads from PREUPDATE / paint
    // don't lag the native control.
    neui_detail::ensure_attrs(child->attrs).set_float(NEUI_PARAM_VALUE, v);

    // Fire VALUE_CHANGED on every notification. TB_ENDTRACK fires once at
    // the end with the final position; the THUMBTRACK / LINE / PAGE codes
    // fire continuously; clients see all of them and can debounce as needed.
    if (child->emit_events) {
      neui_widget_t wid = { child->widget_id };
      neui_event_t ev{};
      ev.type = NEUI_EVENT_VALUE_CHANGED;
      ev.data.value.widget = wid;
      ev.data.value.value  = v;
      sess->dispatch_event(&ev);
    }
    return true;
  }

  // WM_NOTIFY routing (currently TREEVIEW custom-draw + selection). Sets
  // `handled` and returns the wndproc result when consumed; leaves
  // `handled = false` (caller should DefWindowProcW) otherwise.
  LRESULT route_native_notify(Session* sess, LPARAM lParam, bool& handled)
  {
    handled = false;
    if (!sess) return 0;
    NMHDR* hdr = reinterpret_cast<NMHDR*>(lParam);
    uint32_t child_index = static_cast<uint32_t>(hdr->idFrom);
    WidgetData* child = sess->get_widget(child_index);
    if (!child) return 0;

    // Treeview disabled-item handling. Both NM_CUSTOMDRAW (visual
    // graying) and TVN_SELCHANGINGW (selection veto) are behaviour
    // attached to the disabled state; they don't depend on whether
    // the client wants selection events.
    if (child->type && !strcmp(child->type, NEUI_W_TREEVIEW)) {
      if (hdr->code == NM_CUSTOMDRAW) {
        NMTVCUSTOMDRAW* cd = reinterpret_cast<NMTVCUSTOMDRAW*>(lParam);
        bool follow = sess->frame_follows_theme(child);
        handled = true;
        switch (cd->nmcd.dwDrawStage) {
        case CDDS_PREPAINT:
          return CDRF_NOTIFYITEMDRAW;
        case CDDS_ITEMPREPAINT: {
          using neui_detail::ColorRole;
          HTREEITEM hitem = reinterpret_cast<HTREEITEM>(cd->nmcd.dwItemSpec);
          auto rit = child->tree_items_reverse.find(
            reinterpret_cast<uintptr_t>(hitem));
          bool disabled = false;
          if (rit != child->tree_items_reverse.end()) {
            auto it = child->tree_items.find(rit->second);
            disabled = (it != child->tree_items.end() && !it->second.enabled);
          }
          if (follow) {
            bool selected = (cd->nmcd.uItemState & CDIS_SELECTED) != 0;
            if (disabled) {
              cd->clrText   = neui_detail::colorref_from_argb(neui_detail::color(ColorRole::text_disabled));
              cd->clrTextBk = neui_detail::colorref_from_argb(neui_detail::color(ColorRole::control_bg));
            } else if (selected) {
              cd->clrText   = neui_detail::colorref_from_argb(neui_detail::color(ColorRole::accent_text));
              cd->clrTextBk = neui_detail::colorref_from_argb(neui_detail::color(ColorRole::accent));
            } else {
              cd->clrText   = neui_detail::colorref_from_argb(neui_detail::color(ColorRole::text_primary));
              cd->clrTextBk = neui_detail::colorref_from_argb(neui_detail::color(ColorRole::control_bg));
            }
            return CDRF_NEWFONT;
          }
          if (disabled) {
            cd->clrText = GetSysColor(COLOR_GRAYTEXT);
            return CDRF_NEWFONT;
          }
          return CDRF_DODEFAULT;
        }
        }
        return CDRF_DODEFAULT;
      }
      if (hdr->code == TVN_SELCHANGINGW) {
        // Veto selection changes onto disabled items. Runs even when
        // the treeview has emit_events = false; the veto is a state
        // policy, not an event delivery.
        NMTREEVIEWW* ntv = reinterpret_cast<NMTREEVIEWW*>(lParam);
        uint32_t neui_id = static_cast<uint32_t>(ntv->itemNew.lParam);
        auto it = child->tree_items.find(neui_id);
        handled = true;
        if (it != child->tree_items.end() && !it->second.enabled)
          return TRUE;
        return 0;
      }
    }

    if (!child->emit_events) return 0;

    switch (hdr->code)
    {
    case TVN_SELCHANGEDW:
      {
        NMTREEVIEWW* ntv = reinterpret_cast<NMTREEVIEWW*>(lParam);
        uint32_t neui_id = static_cast<uint32_t>(ntv->itemNew.lParam);
        neui_widget_t wid = { child->widget_id };
        neui_event_t ev = { NEUI_EVENT_TREE_ITEM_SELECTED };
        ev.data.tree = { wid, { neui_id } };
        sess->dispatch_event(&ev);
        handled = true;
        return 0;
      }
    case NM_DBLCLK:
      {
        // Activated item is the current selection
        neui_widget_t wid = { child->widget_id };
        neui_item_t   sel = sess->tree_get_selected(wid);
        if (sel.id == tree_item_none.id) { handled = true; return 0; }
        neui_event_t ev = { NEUI_EVENT_TREE_ITEM_ACTIVATED };
        ev.data.tree = { wid, sel };
        sess->dispatch_event(&ev);
        handled = true;
        return 0;
      }
    }
    return 0;
  }

  // Inner body container for a scrolling SECTION. Children of the
  // SECTION HWND-parent here so Win32 clips them naturally to the body
  // rect (no per-child window region needed). Wheel events bubble up
  // through DefWindowProc to the section's painted_msg_fn. The body
  // bg colour is read from the parent section's NEUI_ATTR_BACKGROUND
  // attr on every paint - cheap, and keeps live theme / attr changes
  // reflected without extra plumbing. GWLP_USERDATA holds the section's
  // WidgetData* (set at create time).
  LRESULT CALLBACK SectionBodyWndProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
  {
    // The body HWND's USERDATA holds the owning section's WidgetData* (set at
    // create time); the native-control routing cases below all need its Session.
    auto body_session = [hwnd]() -> Session* {
      auto* sec = reinterpret_cast<WidgetData*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));
      return sec ? sec->session : nullptr;
    };
    switch (msg) {
    case WM_NCCREATE: {
      CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
      if (cs && cs->lpCreateParams)
        SetWindowLongPtr(hwnd, GWLP_USERDATA,
                          reinterpret_cast<LONG_PTR>(cs->lpCreateParams));
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_ERASEBKGND:
      return 1;  // we paint everything in WM_PAINT
    case WM_PAINT: {
      PAINTSTRUCT ps;
      HDC hdc = BeginPaint(hwnd, &ps);
      auto* sec = reinterpret_cast<WidgetData*>(
                    GetWindowLongPtr(hwnd, GWLP_USERDATA));
      using neui_detail::ColorRole;
      uint32_t bg_argb;
      if (sec && sec->attrs && sec->attrs->has(NEUI_ATTR_BACKGROUND)) {
        bg_argb = static_cast<uint32_t>(
                    sec->attrs->get_int(NEUI_ATTR_BACKGROUND, 0));
      } else {
        bg_argb = neui_detail::shade(
                    neui_detail::color(ColorRole::frame_bg),
                    neui_detail::SECTION_BG_LIFT);
      }
      HBRUSH br = neui_detail::brush_from_argb(bg_argb);
      FillRect(hdc, &ps.rcPaint, br);
      DeleteObject(br);
      EndPaint(hwnd, &ps);
      return 0;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
      // Mirror the section's brush handling so STATIC text children
      // (labels, checkbox text) painted onto the body show the body bg
      // colour rather than the system default.
      auto* sec = reinterpret_cast<WidgetData*>(
                    GetWindowLongPtr(hwnd, GWLP_USERDATA));
      if (sec && sec->session) {
        using neui_detail::ColorRole;
        neui_detail::ScopedPaletteOverride scope(
          sec->session->effective_palette_ptr());
        uint32_t bg_argb;
        if (sec->attrs && sec->attrs->has(NEUI_ATTR_BACKGROUND)) {
          bg_argb = static_cast<uint32_t>(
                      sec->attrs->get_int(NEUI_ATTR_BACKGROUND, 0));
        } else {
          bg_argb = neui_detail::shade(
                      neui_detail::color(ColorRole::frame_bg),
                      neui_detail::SECTION_BG_LIFT);
        }
        if (!sec->section_ctl_bg_brush ||
            sec->section_ctl_bg_brush_argb != bg_argb) {
          if (sec->section_ctl_bg_brush) DeleteObject(sec->section_ctl_bg_brush);
          sec->section_ctl_bg_brush      = neui_detail::brush_from_argb(bg_argb);
          sec->section_ctl_bg_brush_argb = bg_argb;
        }
        HDC hdc = reinterpret_cast<HDC>(wParam);
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, neui_detail::colorref_from_argb(
                            neui_detail::color(ColorRole::text_primary)));
        return reinterpret_cast<LRESULT>(sec->section_ctl_bg_brush);
      }
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    // Native-control children re-parented here (slider / button / checkbox /
    // list / combo / treeview) report to THIS body HWND, not the frame. Run
    // the same routing the frame proc does so their events still reach the
    // client - without these cases the controls paint but are silent.
    case WM_HSCROLL:
    case WM_VSCROLL: {
      if (route_native_scroll_notification(body_session(), msg, wParam, lParam))
        return 0;
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_COMMAND: {
      if (route_native_command_notification(body_session(), wParam, lParam))
        return 0;
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    case WM_NOTIFY: {
      bool handled = false;
      LRESULT r = route_native_notify(body_session(), lParam, handled);
      if (handled) return r;
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
  }

  LRESULT CALLBACK AppWindowProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam)
  {
    WidgetData* wd = reinterpret_cast<WidgetData*>(GetWindowLongPtr(hwnd, GWLP_USERDATA));

    // Native menu-bar dark-mode owner-draw via undocumented UAHMENU
    // messages. Only intercept when this frame opted into theme tracking;
    // otherwise let DefWindowProc render the system default light bar.
    if ((msg == neui_detail::k_wm_uah_drawmenu ||
         msg == neui_detail::k_wm_uah_drawmenuitem) &&
        wd && wd->session && wd->session->frame_follows_theme(wd)) {
      // Scope the palette override to this session so the owner-draw
      // helper reads our palette (not a sibling session's last write).
      neui_detail::ScopedPaletteOverride scope(
        wd->session->effective_palette_ptr());
      LRESULT r = 0;
      if (neui_detail::handle_uah_menubar_message(hwnd, msg, wParam, lParam, r))
        return r;
    }

    switch (msg)
    {
    case WM_NCCREATE:
      {
        CREATESTRUCTW* cs = reinterpret_cast<CREATESTRUCTW*>(lParam);
        wd = reinterpret_cast<WidgetData*>(cs->lpCreateParams);
        if (wd) {
          wd->hwnd = hwnd;
          SetWindowLongPtr(hwnd, GWLP_USERDATA, reinterpret_cast<LONG_PTR>(wd));
          if (wd->type && !strcmp(wd->type, NEUI_W_APPWINDOW)) {
            ++g_appwindow_count;
          }
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
      }
    case WM_CREATE:
      {
        if (wd && wd->session) {
          wd->dpi = GetDpiForWindow(hwnd);
          // Apply system theme to the frame if the client opted in. Done
          // before child creation so DWM dark title bar paints correctly
          // on first show; on_theme_changed later re-applies on toggle.
          apply_theme_to_frame_w32(*wd);
          wd->session->create_child_windows(wd->index);
        }
        return 0;
      }
    case WM_INITMENUPOPUP:
      {
        // Auto-disable menu items whose bound built-in command (menu_cmd)
        // can't reach a consumer right now. wParam = HMENU of the popup.
        if (wd && wd->session) {
          HMENU popup = reinterpret_cast<HMENU>(wParam);
          wd->session->update_menu_popup(popup);
          return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
      }
    case WM_COMMAND:
      {
        // Menu pick (HIWORD=0) or accelerator (HIWORD=1) - both have lParam == 0.
        if (wd && wd->session && lParam == 0) {
          wd->session->dispatch_menu_event(LOWORD(wParam));
          return 0;
        }
        // Child control notifications: lParam is the control HWND (non-zero).
        // Shared with SectionBodyWndProc - see route_native_command_notification.
        if (wd && wd->session)
          route_native_command_notification(wd->session, wParam, lParam);
        return 0;
      }
    case WM_HSCROLL:
    case WM_VSCROLL:
      {
        // Trackbar (msctls_trackbar32) reports via WM_HSCROLL/WM_VSCROLL to
        // its parent. Shared with SectionBodyWndProc - see
        // route_native_scroll_notification.
        if (wd && wd->session &&
            route_native_scroll_notification(wd->session, msg, wParam, lParam))
          return 0;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
      }
    case WM_NOTIFY:
      {
        bool handled = false;
        LRESULT r = route_native_notify(wd ? wd->session : nullptr, lParam, handled);
        if (handled) return r;
        return DefWindowProcW(hwnd, msg, wParam, lParam);
      }
    case WM_GETMINMAXINFO:
      {
        if (wd && wd->attrs) {
          MINMAXINFO* mmi = reinterpret_cast<MINMAXINFO*>(lParam);
          UINT dpi = wd->dpi ? wd->dpi : 96;
          int min_w = wd->attrs->get_int(NEUI_ATTR_MIN_WIDTH,  0);
          int min_h = wd->attrs->get_int(NEUI_ATTR_MIN_HEIGHT, 0);
          int max_w = wd->attrs->get_int(NEUI_ATTR_MAX_WIDTH,  0);
          int max_h = wd->attrs->get_int(NEUI_ATTR_MAX_HEIGHT, 0);
          if (min_w > 0) mmi->ptMinTrackSize.x = MulDiv(min_w, static_cast<int>(dpi), 96);
          if (min_h > 0) mmi->ptMinTrackSize.y = MulDiv(min_h, static_cast<int>(dpi), 96);
          if (max_w > 0 && (min_w <= 0 || max_w >= min_w))
            mmi->ptMaxTrackSize.x = MulDiv(max_w, static_cast<int>(dpi), 96);
          if (max_h > 0 && (min_h <= 0 || max_h >= min_h))
            mmi->ptMaxTrackSize.y = MulDiv(max_h, static_cast<int>(dpi), 96);
          return 0;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
      }
    case WM_SIZE:
      {
        // Skip minimised state - Windows reports 0x0 client size and we
        // don't want clients to see spurious zero-sized resize events.
        if (wParam == SIZE_MINIMIZED) return 0;
        if (wd && wd->session) {
          UINT w_phys = LOWORD(lParam);
          UINT h_phys = HIWORD(lParam);
          UINT dpi    = wd->dpi ? wd->dpi : 96;
          int  w_log  = MulDiv(static_cast<int>(w_phys), 96, static_cast<int>(dpi));
          int  h_log  = MulDiv(static_cast<int>(h_phys), 96, static_cast<int>(dpi));
          wd->width   = w_log;
          wd->height  = h_log;

          neui_widget_t wid = { wd->widget_id };
          neui_event_t ev = {};
          ev.type               = NEUI_EVENT_RESIZE;
          ev.data.resize.widget = wid;
          ev.data.resize.width  = w_log;
          ev.data.resize.height = h_log;
          wd->session->dispatch_event(&ev);
        }
        return 0;
      }
    case WM_ERASEBKGND:
      {
        // The window class brush is COLOR_WINDOW+1 (light), which
        // DefWindowProc would happily paint over the whole client area.
        // When the frame opts into the theme, paint the palette panel_bg
        // ourselves so the gaps between native controls match the theme.
        if (wd && wd->session && wd->session->frame_follows_theme(wd)) {
          neui_detail::ScopedPaletteOverride scope(
            wd->session->effective_palette_ptr());
          HDC hdc = reinterpret_cast<HDC>(wParam);
          RECT rc; GetClientRect(hwnd, &rc);
          FillRect(hdc, &rc,
                   neui_detail::brush_for_role(neui_detail::ColorRole::panel_bg));
          return 1;
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
      }
    case WM_SETTINGCHANGE:
      {
        // Top-level frames receive this when system settings change; relay
        // to the theme provider so the palette refreshes and listeners
        // (Sessions) re-invalidate. Belt-and-braces against
        // UISettings::ColorValuesChanged not firing reliably in desktop
        // Win32 apps. Always processed regardless of NEUI_ATTR_FOLLOW_SYSTEM_THEME
        // - the per-frame opt-in only governs whether on_theme_changed
        // applies DWM dark mode + custom-draw to *this* frame.
        const wchar_t* str = reinterpret_cast<const wchar_t*>(lParam);
        if (!str || wcscmp(str, L"ImmersiveColorSet") == 0) {
          neui_detail::refresh_theme_palette_win32();
        }
        return 0;
      }
    case WM_SYSCOLORCHANGE:
      {
        // Classic colour scheme changed - refresh so the GetSysColor-
        // sourced border / scrollbar values follow.
        neui_detail::refresh_theme_palette_win32();
        return 0;
      }
    case WM_CLOSE:
      {
        bool allow = true;
        if (wd && wd->session) {
          neui_widget_t wid = { wd->widget_id };
          neui_event_t event = { NEUI_EVENT_APP_QUIT, { { wid, 0, 0, 0 } } };
          allow = wd->session->dispatch_event(&event);
        }
        if (allow) {
          DestroyWindow(hwnd);
        }
      }
      return 0;
    case WM_DESTROY:
      {
        if (wd && wd->type && !strcmp(wd->type, NEUI_W_APPWINDOW)) {
          --g_appwindow_count;
          if (g_appwindow_count <= 0) {
            PostQuitMessage(0);
          }
        }
        // Auto-close any frames owned by this one. Clear our own hwnd
        // first so the owned dialogs' teardown skips the "re-enable
        // owner" path (the owner is going away). DestroyWindow re-enters
        // WM_DESTROY synchronously for each owned frame.
        if (wd && wd->session) {
          uint32_t self_idx = wd->index;
          wd->hwnd = nullptr;
          wd->session->close_owned_frames(self_idx);
        }
        // Dialog teardown: re-enable and re-activate the owner so input
        // returns there. Skip when the dialog opted into modeless
        // (NEUI_ATTR_MODAL = 0) - the owner was never disabled and we
        // shouldn't steal its activation. Dialogs do not affect
        // g_appwindow_count.
        if (wd && wd->type && !strcmp(wd->type, NEUI_W_DIALOG) &&
            wd->owner_index != 0 && wd->session) {
          bool is_modal = !wd->attrs ||
                          wd->attrs->get_int(NEUI_ATTR_MODAL, 1) != 0;
          WidgetData* owner = wd->session->get_widget(wd->owner_index);
          if (is_modal && owner && owner->hwnd) {
            EnableWindow(owner->hwnd, TRUE);
            SetForegroundWindow(owner->hwnd);
          }
          // Drop the modal pump so widget_show unwinds and returns.
          if (wd) wd->modal_pump_active = false;
        }
        return 0;
      }
    case WM_DPICHANGED:
      {
        if (wd) {
          UINT new_dpi = HIWORD(wParam);
          wd->dpi = new_dpi;
          // Recreate DPI font and reapply to all children
          if (wd->hfont) {
            DeleteObject(wd->hfont);
            wd->hfont = nullptr;
          }
          if (wd->session) {
            // Resize every descendant HWND to the new physical px first,
            // then run create_child_windows to refresh fonts. Order matters:
            // create_child_windows reapplies WM_SETFONT to existing HWNDs,
            // which must already be at the right size for text to lay out.
            wd->session->cascade_dpi(wd->index, new_dpi);
            wd->session->create_child_windows(wd->index);
          }
          const RECT* r = reinterpret_cast<const RECT*>(lParam);
          SetWindowPos(hwnd, nullptr,
            r->left, r->top,
            r->right - r->left, r->bottom - r->top,
            SWP_NOZORDER | SWP_NOACTIVATE);
        }
        return 0;
      }
    case WM_CTLCOLORSTATIC:
      {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND child = reinterpret_cast<HWND>(lParam);
        WidgetData* child_wd = widget_for_child_hwnd_w32(wd, child);
        if (wd && wd->session &&
            wd->session->frame_follows_theme(child_wd ? child_wd : wd)) {
          using neui_detail::ColorRole;
          neui_detail::ScopedPaletteOverride scope(
            wd->session->effective_palette_ptr());
          SetBkMode(hdc, TRANSPARENT);
          SetTextColor(hdc, neui_detail::colorref_from_argb(
                              neui_detail::color(ColorRole::text_primary)));
          return reinterpret_cast<LRESULT>(neui_detail::brush_for_role(ColorRole::panel_bg));
        }
        SetBkMode(hdc, TRANSPARENT);
        return reinterpret_cast<LRESULT>(GetSysColorBrush(COLOR_WINDOW));
      }
    case WM_CTLCOLOREDIT:
      {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND child = reinterpret_cast<HWND>(lParam);
        WidgetData* child_wd = widget_for_child_hwnd_w32(wd, child);
        if (wd && wd->session &&
            wd->session->frame_follows_theme(child_wd ? child_wd : wd)) {
          using neui_detail::ColorRole;
          neui_detail::ScopedPaletteOverride scope(
            wd->session->effective_palette_ptr());
          SetBkColor(hdc,   neui_detail::colorref_from_argb(neui_detail::color(ColorRole::control_bg)));
          SetTextColor(hdc, neui_detail::colorref_from_argb(neui_detail::color(ColorRole::text_primary)));
          return reinterpret_cast<LRESULT>(neui_detail::brush_for_role(ColorRole::control_bg));
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
      }
    case WM_CTLCOLORLISTBOX:
      {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND child = reinterpret_cast<HWND>(lParam);
        WidgetData* child_wd = widget_for_child_hwnd_w32(wd, child);
        if (wd && wd->session &&
            wd->session->frame_follows_theme(child_wd ? child_wd : wd)) {
          using neui_detail::ColorRole;
          neui_detail::ScopedPaletteOverride scope(
            wd->session->effective_palette_ptr());
          SetBkColor(hdc,   neui_detail::colorref_from_argb(neui_detail::color(ColorRole::control_bg)));
          SetTextColor(hdc, neui_detail::colorref_from_argb(neui_detail::color(ColorRole::text_primary)));
          return reinterpret_cast<LRESULT>(neui_detail::brush_for_role(ColorRole::control_bg));
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
      }
    case WM_CTLCOLORBTN:
      {
        HDC hdc = reinterpret_cast<HDC>(wParam);
        HWND child = reinterpret_cast<HWND>(lParam);
        WidgetData* child_wd = widget_for_child_hwnd_w32(wd, child);
        if (wd && wd->session &&
            wd->session->frame_follows_theme(child_wd ? child_wd : wd)) {
          using neui_detail::ColorRole;
          neui_detail::ScopedPaletteOverride scope(
            wd->session->effective_palette_ptr());
          SetBkMode(hdc, TRANSPARENT);
          SetTextColor(hdc, neui_detail::colorref_from_argb(neui_detail::color(ColorRole::text_primary)));
          return reinterpret_cast<LRESULT>(neui_detail::brush_for_role(ColorRole::panel_bg));
        }
        return DefWindowProcW(hwnd, msg, wParam, lParam);
      }
    default:
      return DefWindowProcW(hwnd, msg, wParam, lParam);
    }
  }

  // True if this widget is a native EDIT-backed text widget eligible for
  // multi-level undo tracking. Password fields opt out: storing plaintext
  // pre-states in the EditHistory would leak the value to anyone who can
  // walk the heap.
  static bool is_history_text_widget_w32(WidgetData* wd)
  {
    if (!wd || !wd->type) return false;
    if (strcmp(wd->type, NEUI_W_INPUTBOX) != 0 &&
        strcmp(wd->type, NEUI_W_MULTILINE) != 0) return false;
    if (wd->attrs && wd->attrs->get_int(NEUI_ATTR_PASSWORD, 0)) return false;
    return true;
  }

  // Read EDIT text + selection into an EditState. Text is UTF-8 (consistent
  // with the rest of the framework); cursor/anchor are UTF-16 code unit
  // indices (what EM_GETSEL / EM_SETSEL speak natively). EditHistory treats
  // them as opaque, so no conversion is needed to keep them in sync with
  // the EDIT control.
  static void capture_edit_state_w32(HWND hwnd, neui_detail::EditState& out)
  {
    int wlen = GetWindowTextLengthW(hwnd);
    if (wlen > 0) {
      std::wstring wtext(wlen, L'\0');
      GetWindowTextW(hwnd, &wtext[0], wlen + 1);
      int u8len = WideCharToMultiByte(CP_UTF8, 0, wtext.c_str(), wlen,
                                       nullptr, 0, nullptr, nullptr);
      out.text.assign((size_t)u8len, '\0');
      if (u8len > 0)
        WideCharToMultiByte(CP_UTF8, 0, wtext.c_str(), wlen,
                            &out.text[0], u8len, nullptr, nullptr);
    } else {
      out.text.clear();
    }
    DWORD start = 0, end = 0;
    SendMessageW(hwnd, EM_GETSEL, (WPARAM)&start, (LPARAM)&end);
    out.anchor = (int)start;
    out.cursor = (int)end;
  }

  // Push a state back into the EDIT control. Bracketed in WM_SETREDRAW so
  // text + selection swap in a single repaint rather than flashing the
  // intermediate "selection at end of new text" state.
  static void apply_edit_state_w32(HWND hwnd, const neui_detail::EditState& s)
  {
    int u8len = (int)s.text.size();
    int wlen = (u8len > 0)
      ? MultiByteToWideChar(CP_UTF8, 0, s.text.c_str(), u8len, nullptr, 0)
      : 0;
    std::wstring wtext((size_t)wlen, L'\0');
    if (wlen > 0)
      MultiByteToWideChar(CP_UTF8, 0, s.text.c_str(), u8len, &wtext[0], wlen);
    SendMessageW(hwnd, WM_SETREDRAW, FALSE, 0);
    SetWindowTextW(hwnd, wtext.c_str());
    SendMessageW(hwnd, EM_SETSEL, (WPARAM)s.anchor, (LPARAM)s.cursor);
    SendMessageW(hwnd, WM_SETREDRAW, TRUE, 0);
    InvalidateRect(hwnd, nullptr, TRUE);
  }

  // Record the current EDIT state as a pre-edit snapshot, BEFORE the
  // mutating message reaches the EDIT control. EditHistory coalesces
  // consecutive same-kind marks (typing / deleting) when no selection is
  // involved, so this is cheap to call on every keystroke.
  static void track_edit_mutation_w32(WidgetData& wd, HWND hwnd,
                                       neui_detail::EditHistory::Action kind)
  {
    if (!is_history_text_widget_w32(&wd)) return;
    if (!wd.edit_history) wd.edit_history.reset(new neui_detail::EditHistory());
    neui_detail::EditState pre;
    capture_edit_state_w32(hwnd, pre);
    bool has_sel = pre.anchor != pre.cursor;
    wd.edit_history->mark(pre, kind, has_sel);
  }

  bool try_edit_undo_redo_w32(WidgetData& wd, HWND hwnd, bool redo)
  {
    if (!wd.edit_history) return false;
    neui_detail::EditState current, restored;
    capture_edit_state_w32(hwnd, current);
    bool ok = redo
      ? wd.edit_history->redo(current, restored)
      : wd.edit_history->undo(current, restored);
    if (!ok) return false;
    apply_edit_state_w32(hwnd, restored);
    return true;
  }

  LRESULT CALLBACK ChildSubclassProc(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam, UINT_PTR uIdSubclass, DWORD_PTR dwRefData)
  {
    WidgetData* wd = reinterpret_cast<WidgetData*>(dwRefData);

    if (wd && wd->emit_events && wd->session) {
      UINT dpi = wd->session->get_dpi_for_widget(wd->index);
      // Translate physical coords to logical for mouse messages
      int lx = MulDiv(GET_X_LPARAM(lParam), 96, (int)dpi);
      int ly = MulDiv(GET_Y_LPARAM(lParam), 96, (int)dpi);
      neui_widget_t wid = { wd->widget_id };

      switch (msg)
      {
      case WM_MOUSEMOVE:
        if (!wd->mouse_tracked) {
          TRACKMOUSEEVENT tme = {};
          tme.cbSize    = sizeof(tme);
          tme.dwFlags   = TME_LEAVE;
          tme.hwndTrack = hwnd;
          TrackMouseEvent(&tme);
          wd->mouse_tracked = true;
          neui_event_t enter = { NEUI_EVENT_MOUSE_ENTER };
          enter.data.mouse = { wid, 0, 0, 0 };
          wd->session->dispatch_event(&enter);
        }
        {
          neui_event_t event = { NEUI_EVENT_MOUSE_MOVE };
          event.data.mouse = { wid, lx, ly, 0 };
          wd->session->dispatch_event(&event);
        }
        break;
      case WM_MOUSELEAVE:
        wd->mouse_tracked = false;
        {
          neui_event_t event = { NEUI_EVENT_MOUSE_LEAVE };
          event.data.mouse = { wid, 0, 0, 0 };
          wd->session->dispatch_event(&event);
        }
        break;
      case WM_LBUTTONDOWN:
        {
          neui_event_t event = { NEUI_EVENT_MOUSE_BUTTON_DOWN };
          event.data.mouse = { wid, lx, ly, 1 };
          wd->session->dispatch_event(&event);
        }
        break;
      case WM_LBUTTONUP:
        {
          neui_event_t event = { NEUI_EVENT_MOUSE_BUTTON_UP };
          event.data.mouse = { wid, lx, ly, 0 };
          wd->session->dispatch_event(&event);
        }
        break;
      case WM_LBUTTONDBLCLK:
        {
          neui_event_t event = { NEUI_EVENT_MOUSE_BUTTON_DBLCLICK };
          event.data.mouse = { wid, lx, ly, 1 };
          wd->session->dispatch_event(&event);
          // Slider double-click -> reset to NEUI_PARAM_DEFAULT. Knob has its
          // own DBLCLK handling in painted_msg_knob_w32; only the native
          // trackbar needs framework-side intervention here.
          if (wd->type && !strcmp(wd->type, NEUI_W_SLIDER))
            widget_reset_to_default_w32(*wd);
        }
        break;
      case WM_RBUTTONDOWN:
        {
          neui_event_t event = { NEUI_EVENT_MOUSE_RBUTTON_DOWN };
          event.data.mouse = { wid, lx, ly, 0 };
          wd->session->dispatch_event(&event);
        }
        break;
      case WM_RBUTTONUP:
        {
          neui_event_t event = { NEUI_EVENT_MOUSE_RBUTTON_UP };
          event.data.mouse = { wid, lx, ly, 0 };
          wd->session->dispatch_event(&event);
        }
        break;
      case WM_KEYDOWN:
        {
          neui_event_t event = { NEUI_EVENT_KEYDOWN };
          event.data.key = { wid, static_cast<uint32_t>(wParam), 0 };
          wd->session->dispatch_event(&event);
        }
        break;
      case WM_KEYUP:
        {
          neui_event_t event = { NEUI_EVENT_KEYUP };
          event.data.key = { wid, static_cast<uint32_t>(wParam), 0 };
          wd->session->dispatch_event(&event);
        }
        break;
      case WM_CHAR:
        {
          wchar_t ch = static_cast<wchar_t>(wParam);
          uint32_t codepoint = 0;

          if (ch >= 0xD800 && ch <= 0xDBFF) {
            // High surrogate: store and wait for the paired low surrogate
            wd->pending_surrogate = ch;
            break;
          } else if (ch >= 0xDC00 && ch <= 0xDFFF) {
            // Low surrogate: combine with stored high surrogate into a code point
            if (wd->pending_surrogate != 0) {
              codepoint = 0x10000u
                + ((wd->pending_surrogate - 0xD800u) << 10)
                + (ch - 0xDC00u);
              wd->pending_surrogate = 0;
            } else {
              break;  // orphan low surrogate - discard
            }
          } else {
            wd->pending_surrogate = 0;
            codepoint = ch;  // BMP character, code unit == code point
          }

          neui_event_t event = { NEUI_EVENT_KEYCHAR };
          event.data.key = { wid, codepoint, 0 };
          wd->session->dispatch_event(&event);
        }
        break;
      case WM_SETFOCUS:
        {
          neui_event_t event = { NEUI_EVENT_WIDGET_FOCUS };
          event.data.focus = { wid, true };
          wd->session->dispatch_event(&event);
          // Auto-scroll any enclosing scrolling SECTION ancestor so a
          // Tab-into-off-screen child brings it into view.
          wd->session->ensure_widget_visible(wd->index);
        }
        break;
      case WM_KILLFOCUS:
        {
          neui_event_t event = { NEUI_EVENT_WIDGET_FOCUS };
          event.data.focus = { wid, false };
          wd->session->dispatch_event(&event);
        }
        break;
      default:
        break;
      }
    }

    // EditHistory tracking for native EDIT-backed widgets (INPUTBOX /
    // MULTILINE). Runs AFTER the client has seen NEUI_EVENT_KEY* above and
    // BEFORE the EDIT mutates, so the snapshot captures pre-edit state.
    // Ctrl+Z / Ctrl+Y are intercepted here so the framework's multi-level
    // history works regardless of whether the client wired a menu binding
    // - matching xpl's InputBoxWidget::on_keydown behavior.
    if (is_history_text_widget_w32(wd)) {
      using neui_detail::EditHistory;
      switch (msg) {
      case WM_KEYDOWN: {
        bool ctrl  = (GetKeyState(VK_CONTROL) & 0x8000) != 0;
        bool shift = (GetKeyState(VK_SHIFT)   & 0x8000) != 0;
        if (ctrl && (wParam == 'Z' || wParam == 'Y')) {
          // Ctrl+Z = undo, Ctrl+Shift+Z / Ctrl+Y = redo. Returning 0
          // suppresses DefSubclassProc so the native EDIT's own EM_UNDO
          // single-level toggle never runs.
          bool redo = (wParam == 'Y') || (wParam == 'Z' && shift);
          if (try_edit_undo_redo_w32(*wd, hwnd, redo)) return 0;
          // No history to replay - still consume so EM_UNDO doesn't fire.
          return 0;
        }
        if (wParam == VK_DELETE) {
          track_edit_mutation_w32(*wd, hwnd, EditHistory::Deleting);
        } else if (wParam == VK_LEFT || wParam == VK_RIGHT ||
                   wParam == VK_UP   || wParam == VK_DOWN  ||
                   wParam == VK_HOME || wParam == VK_END   ||
                   wParam == VK_PRIOR || wParam == VK_NEXT) {
          if (wd->edit_history) wd->edit_history->reset_action();
        }
        break;
      }
      case WM_CHAR: {
        wchar_t ch = (wchar_t)wParam;
        if (ch == 0x08 || ch == 0x7F) {
          // 0x08 = backspace, 0x7F = Ctrl+Backspace (word delete).
          track_edit_mutation_w32(*wd, hwnd, EditHistory::Deleting);
        } else if (ch >= 0x20 || ch == 0x0D) {
          // Printable or CR (newline in multiline). High/low surrogate
          // pairs both pass; EDIT assembles them on its own. Skips pure
          // control chars (Tab navigates, Esc cancels, etc).
          track_edit_mutation_w32(*wd, hwnd, EditHistory::Typing);
        }
        break;
      }
      case WM_PASTE:
      case WM_CUT:
      case WM_CLEAR:
        // Single-step mutations. Action=None defeats coalescing so each
        // paste / cut / delete becomes its own undo entry.
        track_edit_mutation_w32(*wd, hwnd, EditHistory::None);
        break;
      case WM_LBUTTONDOWN:
      case WM_RBUTTONDOWN:
      case WM_SETFOCUS:
      case WM_KILLFOCUS:
        // Caret jump / focus change breaks the typing run so the next
        // character starts a fresh undo group.
        if (wd->edit_history) wd->edit_history->reset_action();
        break;
      default:
        break;
      }
    }

    return DefSubclassProc(hwnd, msg, wParam, lParam);
  }

  // Process one Win32 message and run the standard accelerator / dialog
  // / dispatch sequence. Returns the GetMessage / dispatch outcome:
  //   true  - message dispatched, keep going
  //   false - WM_QUIT received (msg.message == WM_QUIT), stop the loop
  static bool dispatch_one_message(MSG& msg)
  {
    if (msg.message == WM_QUIT) return false;

    // Menu accelerator translation runs before tab/dialog handling so a
    // bound shortcut takes priority over widget-local keystrokes.
    for (auto& s : sessions) {
      if (s && s->try_translate_accel(&msg)) return true;
    }

    // Route Tab/Shift+Tab through the dialog manager so WS_TABSTOP controls
    // cycle focus in creation (widget tree) order.
    HWND root = msg.hwnd ? GetAncestor(msg.hwnd, GA_ROOT) : nullptr;
    if (root && IsDialogMessage(root, &msg))
      return true;

    ::TranslateMessage(&msg);
    ::DispatchMessage(&msg);
    return true;
  }

  bool run()
  {
    BOOL bRet;
    MSG msg;
    while ((bRet = ::GetMessage(&msg, NULL, 0, 0)) != 0)
    {
      if (bRet == -1)
        return false;
      if (!dispatch_one_message(msg)) return true;
    }
    return true;
  }

  // Drain pending Win32 messages without blocking. Returns false when
  // WM_QUIT was seen (caller should stop their loop), true otherwise.
  bool pump_once()
  {
    MSG msg;
    while (::PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
      if (!dispatch_one_message(msg)) return false;
    }
    return true;
  }

  // Nested pump for blocking modal DIALOG show. Same dispatch rules as
  // run() (accelerator + IsDialogMessage); exits when the dialog's
  // WM_DESTROY clears *keep_running or when WM_QUIT arrives.
  void run_modal_until(bool* keep_running)
  {
    if (!keep_running) return;
    MSG msg;
    while (*keep_running) {
      BOOL bRet = ::GetMessage(&msg, NULL, 0, 0);
      if (bRet == 0) {
        // WM_QUIT - re-post so the outer pump still sees it.
        ::PostQuitMessage(static_cast<int>(msg.wParam));
        return;
      }
      if (bRet == -1) return;
      if (!dispatch_one_message(msg)) {
        ::PostQuitMessage(0);
        return;
      }
    }
  }

} // namespace win32_host

int WINAPI WinMain(HINSTANCE hInstance, HINSTANCE hPrevInstance, LPSTR lpCmdLine, int nCmdShow)
{
  SetProcessDpiAwarenessContext(DPI_AWARENESS_CONTEXT_PER_MONITOR_AWARE_V2);
  CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);  // required for WIC image loading
  win32_host::set_hinstance(hInstance);
  win32_host::register_classes();
  win32_host::register_host();
  int argc;
  wchar_t** argv = CommandLineToArgvW(GetCommandLineW(), &argc);
  return main(argc, argv);
}
