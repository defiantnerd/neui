#pragma once

#ifdef _WIN32

#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif

#include <windows.h>
#include "../theme_palette.h"
#include "theme_brushes_win32.h"

// Native menu bar (the "File Edit Help" strip across the top of a frame)
// dark-mode painting via undocumented UAHMENU messages.
//
// AllowDarkModeForWindow handles the popup dropdowns (drawn as themed
// "Menu" controls), but the menu bar strip itself is non-client area
// rendered by the window manager and ignores AllowDarkModeForWindow on
// every Windows build to date. The work-around used by File Explorer,
// Settings, Notepad, and Windows Terminal is to handle two
// undocumented but stable messages:
//
//   WM_UAHDRAWMENU      (0x0091) - paint the menu-bar background
//   WM_UAHDRAWMENUITEM  (0x0092) - paint each "File", "Edit", ... item
//
// These messages have shipped unchanged since Windows 10 1809. The
// structs they use (UAHMENU, UAHDRAWMENUITEM, ...) are likewise stable
// even though Microsoft never published headers for them.

namespace neui_detail
{
  static constexpr UINT k_wm_uah_drawmenu     = 0x0091;
  static constexpr UINT k_wm_uah_drawmenuitem = 0x0092;

  typedef union UAHMENUITEMMETRICS_NEUI {
    struct { DWORD cx; DWORD cy; } rgsizeBar[2];
    struct { DWORD cx; DWORD cy; } rgsizePopup[4];
  } UAHMENUITEMMETRICS_NEUI;

  typedef struct UAHMENUPOPUPMETRICS_NEUI {
    DWORD rgcx[4];
    DWORD fUpdateMaxWidths : 2;
  } UAHMENUPOPUPMETRICS_NEUI;

  typedef struct UAHMENU_NEUI {
    HMENU hmenu;
    HDC   hdc;
    DWORD dwFlags;
  } UAHMENU_NEUI;

  typedef struct UAHMENUITEM_NEUI {
    int                       iPosition;
    UAHMENUITEMMETRICS_NEUI   umim;
    UAHMENUPOPUPMETRICS_NEUI  umpm;
  } UAHMENUITEM_NEUI;

  typedef struct UAHDRAWMENUITEM_NEUI {
    DRAWITEMSTRUCT     dis;
    UAHMENU_NEUI       um;
    UAHMENUITEM_NEUI   umi;
  } UAHDRAWMENUITEM_NEUI;

  // Returns true if the message was handled and `out` carries the
  // window proc's return value. Frame WndProcs should consult this
  // before calling DefWindowProc.
  inline bool handle_uah_menubar_message(HWND hwnd, UINT msg,
                                          WPARAM /*wParam*/, LPARAM lParam,
                                          LRESULT& out)
  {
    if (msg == k_wm_uah_drawmenu) {
      auto* udm = reinterpret_cast<UAHMENU_NEUI*>(lParam);
      if (!udm || !udm->hdc) { out = 0; return true; }
      // Paint the entire menu-bar rect with the palette frame_bg.
      MENUBARINFO mbi = {};
      mbi.cbSize = sizeof(mbi);
      if (GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) {
        RECT rcWindow;
        GetWindowRect(hwnd, &rcWindow);
        RECT rcBar = mbi.rcBar;
        OffsetRect(&rcBar, -rcWindow.left, -rcWindow.top);
        HBRUSH hbr = brush_for_role(ColorRole::frame_bg);
        FillRect(udm->hdc, &rcBar, hbr);
      }
      out = 0;
      return true;
    }

    if (msg == k_wm_uah_drawmenuitem) {
      auto* udmi = reinterpret_cast<UAHDRAWMENUITEM_NEUI*>(lParam);
      if (!udmi || !udmi->dis.hDC) { out = 0; return true; }

      // Read the item's text via the standard menu API.
      wchar_t text[256] = {};
      MENUITEMINFOW mii = {};
      mii.cbSize     = sizeof(mii);
      mii.fMask      = MIIM_STRING;
      mii.dwTypeData = text;
      mii.cch        = 255;
      GetMenuItemInfoW(udmi->um.hmenu, udmi->umi.iPosition, TRUE, &mii);

      const bool selected = (udmi->dis.itemState & ODS_SELECTED) != 0;
      const bool hot      = (udmi->dis.itemState & ODS_HOTLIGHT) != 0;
      const bool disabled = (udmi->dis.itemState & (ODS_INACTIVE | ODS_GRAYED)) != 0;

      // Background - accent for hot/selected, frame_bg otherwise.
      HBRUSH bg_brush = (selected || hot)
                        ? brush_for_role(ColorRole::accent)
                        : brush_for_role(ColorRole::frame_bg);
      FillRect(udmi->dis.hDC, &udmi->dis.rcItem, bg_brush);

      // Text - accent_text on hot/selected, text_disabled when greyed,
      // otherwise text_primary.
      const Palette& pal = current_palette();
      COLORREF text_col;
      if (disabled)
        text_col = colorref_from_argb(color(pal, ColorRole::text_disabled));
      else if (selected || hot)
        text_col = colorref_from_argb(color(pal, ColorRole::accent_text));
      else
        text_col = colorref_from_argb(color(pal, ColorRole::text_primary));

      SetBkMode(udmi->dis.hDC, TRANSPARENT);
      SetTextColor(udmi->dis.hDC, text_col);
      DrawTextW(udmi->dis.hDC, text, -1, &udmi->dis.rcItem,
                DT_CENTER | DT_VCENTER | DT_SINGLELINE);

      out = 0;
      return true;
    }

    return false;
  }

} // namespace neui_detail

#endif // _WIN32
