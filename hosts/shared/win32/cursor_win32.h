#pragma once

#define NOMINMAX
#define WIN32_LEAN_AND_MEAN
#include <windows.h>

#include "../cursor_kind.h"

// neui_cursor_kind -> HCURSOR, shared by BOTH win32 hosts (the native host's
// GRID column-resize feedback and the xpl host's platform_set_cursor /
// WM_SETCURSOR handler). Previously each kept its own two-entry switch, one of
// them annotated "Mirrors xpl_host::CursorKind values" - the same drift risk
// keys_win32.h was created to remove.
//
// Win32 has a stock cursor for almost every kind. The two it lacks:
//   - open / closed hand: no stock equivalent, both map to IDC_HAND. A client
//     that wants a real grab cursor ships its own (out of scope here).
//   - "none": not a shape but a MODE - SetCursor(NULL) hides the pointer, so
//     the caller must special-case it rather than looking up a handle.
//
// Stock cursors are shared, never destroyed, and LoadCursorW is a cheap cached
// lookup after the first call, so there is nothing to cache here.

namespace neui_detail
{

  // The stock cursor for a kind. Returns nullptr for NEUI_CURSOR_NONE (hide)
  // so callers can distinguish "hide the pointer" from "lookup failed" via
  // cursor_kind_is_hidden().
  inline HCURSOR win32_cursor_for_kind(int kind)
  {
    // IDC_* are LPCSTR resource ids; the cast keeps them usable with the W
    // variant, which ignores char width for the resource-id (ordinal) form.
    LPCWSTR id = nullptr;
    switch (kind) {
      case NEUI_CURSOR_IBEAM:       id = (LPCWSTR)IDC_IBEAM;       break;
      case NEUI_CURSOR_CROSSHAIR:   id = (LPCWSTR)IDC_CROSS;       break;
      case NEUI_CURSOR_HAND:
      case NEUI_CURSOR_OPEN_HAND:                 // no stock open-hand
      case NEUI_CURSOR_CLOSED_HAND: id = (LPCWSTR)IDC_HAND;        break;
      case NEUI_CURSOR_EW_RESIZE:   id = (LPCWSTR)IDC_SIZEWE;      break;
      case NEUI_CURSOR_NS_RESIZE:   id = (LPCWSTR)IDC_SIZENS;      break;
      case NEUI_CURSOR_NESW_RESIZE: id = (LPCWSTR)IDC_SIZENESW;    break;
      case NEUI_CURSOR_NWSE_RESIZE: id = (LPCWSTR)IDC_SIZENWSE;    break;
      case NEUI_CURSOR_MOVE:        id = (LPCWSTR)IDC_SIZEALL;     break;
      case NEUI_CURSOR_WAIT:        id = (LPCWSTR)IDC_WAIT;        break;
      case NEUI_CURSOR_PROGRESS:    id = (LPCWSTR)IDC_APPSTARTING; break;
      case NEUI_CURSOR_HELP:        id = (LPCWSTR)IDC_HELP;        break;
      case NEUI_CURSOR_NOT_ALLOWED: id = (LPCWSTR)IDC_NO;          break;
      case NEUI_CURSOR_NONE:        return nullptr;                // hide
      case NEUI_CURSOR_ARROW:
      case NEUI_CURSOR_DEFAULT:
      default:                      id = (LPCWSTR)IDC_ARROW;       break;
    }
    return LoadCursorW(nullptr, id);
  }

  // Apply a kind now. Separated from the lookup because NONE is a mode:
  // SetCursor(nullptr) is the documented way to hide, and passing a null
  // handle through from a *failed* lookup would hide the pointer by accident,
  // so the hidden case is decided by the kind and never by the handle.
  inline void win32_apply_cursor(int kind)
  {
    if (cursor_kind_is_hidden(kind)) { SetCursor(nullptr); return; }
    if (HCURSOR c = win32_cursor_for_kind(kind)) SetCursor(c);
  }

} // namespace neui_detail
