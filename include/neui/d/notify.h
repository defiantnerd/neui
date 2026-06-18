#pragma once

#include <stdint.h>
#include "api.h"

#ifdef __cplusplus
extern "C" {
#endif

// User notification surfaces rendered by the host outside the widget tree:
// transient toasts and modal message boxes. Neither call creates a widget -
// no handle is returned, no tree slot is occupied, no attrs or events
// attach. The parent_window parameter is an anchor/owner only (the same
// role the source widget plays in dnd->begin_drag).
//
// A host without a notification story simply does not expose this
// interface; clients feature-detect via get_interface returning NULL.
#define NEUI_API_NOTIFY "com.defiantnerd.neui.extension.notify/0"

// Message box flags. Values match the Win32 MB_* constants numerically
// (house convention, cf. neui_dnd_action_t == DROPEFFECT_*), so the win32
// native host passes them through to MessageBoxExW unchanged.
//
// Button set (mutually exclusive, low nibble):
#define NEUI_MB_OK                0x0000u
#define NEUI_MB_OKCANCEL          0x0001u
#define NEUI_MB_ABORTRETRYIGNORE  0x0002u
#define NEUI_MB_YESNOCANCEL       0x0003u
#define NEUI_MB_YESNO             0x0004u
#define NEUI_MB_RETRYCANCEL       0x0005u
#define NEUI_MB_CANCELTRYCONTINUE 0x0006u
// Icon (mutually exclusive, second nibble):
#define NEUI_MB_ICONERROR         0x0010u
#define NEUI_MB_ICONQUESTION      0x0020u
#define NEUI_MB_ICONWARNING       0x0030u
#define NEUI_MB_ICONINFORMATION   0x0040u
// Default button (mutually exclusive, third nibble):
#define NEUI_MB_DEFBUTTON1        0x0000u
#define NEUI_MB_DEFBUTTON2        0x0100u
#define NEUI_MB_DEFBUTTON3        0x0200u

// message_box return values. Values match the Win32 ID* constants
// numerically. 0 = failure / unsupported (no host surface, bad widget).
#define NEUI_ID_OK        1
#define NEUI_ID_CANCEL    2
#define NEUI_ID_ABORT     3
#define NEUI_ID_RETRY     4
#define NEUI_ID_IGNORE    5
#define NEUI_ID_YES       6
#define NEUI_ID_NO        7
#define NEUI_ID_TRYAGAIN  10
#define NEUI_ID_CONTINUE  11

typedef struct neui_notify_api
{
  uint32_t neui_version;

  // Show a transient notification ("toast") over the client area of a
  // top-level frame. `parent_window` must be an APPWINDOW, PLUGWINDOW, or
  // DIALOG widget; the toast is anchored to that frame's client-area top
  // and slides into place from above. `text` is a UTF-8 string; newline
  // characters ('\n') separate display lines and the host measures the
  // widest line to size the toast box. The toast requires no user
  // interaction and dismisses itself after roughly 2 seconds; a click on
  // it dismisses immediately. A second call on the same frame while one
  // is still showing replaces the previous toast. Per-host feel varies:
  // xpl + macOS animate a slide + fade in/out; win32 native appears fast
  // and fades out slowly. No-op if the parent is not a frame or the host
  // has no surface to paint on.
  void (NEUI_ABI *toast)(neui_session_t session, neui_widget_t parent_window,
                         const char* text);

  // Show a modal message box owned by `parent_window` (APPWINDOW,
  // PLUGWINDOW, or DIALOG). Mimics Win32 MessageBoxEx: `text` is the
  // body, `caption` the title bar (NULL = host default), `flags` a
  // combination of one NEUI_MB_* button set, one icon, and one default
  // button. Blocks the calling thread until the user picks a button; the
  // owner is disabled while shown (same modal contract as a
  // NEUI_ATTR_MODAL dialog). Returns the NEUI_ID_* of the chosen button,
  // or 0 on failure (bad widget, cross-session handle, no host surface).
  // On win32 (native AND xpl) this is MessageBoxExW with a neutral
  // language id; macOS maps to NSAlert run modally; the null platform
  // returns 0.
  //
  // iOS DIVERGENCE: UIAlertController is inherently asynchronous (it is
  // presented, not run modally), so the synchronous "blocks and returns
  // the chosen NEUI_ID_*" contract does NOT hold on iOS. The iOS host
  // presents the alert (buttons appear + dismiss correctly on tap) and
  // returns immediately with the sentinel NEUI_MB_IOS_PENDING (-1, defined
  // in the host's message_box_ios.h; distinct from every NEUI_ID_* and from
  // the 0 failure return). Delivering the chosen button back to the client
  // (a completion callback / event) is a deferred follow-up; for now treat
  // NEUI_MB_IOS_PENDING as "shown, result not yet available".
  int  (NEUI_ABI *message_box)(neui_session_t session,
                               neui_widget_t parent_window,
                               const char* text, const char* caption,
                               uint32_t flags);
} neui_notify_api_t;

#ifdef __cplusplus
}
#endif
