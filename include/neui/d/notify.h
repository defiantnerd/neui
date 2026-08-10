#pragma once

#include <stdint.h>
#include "api.h"
// neui_widget_t (every entry point here is anchored to a frame). Pulled in
// directly rather than relying on neui.h's include order, so this header is
// usable standalone - hosts/shared/file_dialog_model.h includes just this one.
#include "events.h"

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

// ---------------------------------------------------------------------------
// File dialogs (open_file / save_file)
// ---------------------------------------------------------------------------

// One entry in the dialog's file-type list.
//
// `label` is what the user reads ("PNG images"); `patterns` is a
// semicolon-separated glob list ("*.png;*.PNG"). Only `*` (any run,
// including empty) and `?` (exactly one character) are special - the
// matcher is deliberately simpler than a shell glob (no character
// classes, no brace expansion) because every native dialog reduces the
// pattern to an extension set anyway. Matching is ASCII
// case-INSENSITIVE on every platform, so "*.png" also accepts "IMG.PNG"
// on Linux; this is a deliberate divergence from the case-sensitive
// filesystem, matching what users expect of a file-type filter.
//
// `"*"` (or `"*.*"`) means "every file" and is the conventional way to
// offer an escape hatch alongside the typed entries.
typedef struct neui_file_filter
{
  const char* label;
  const char* patterns;
} neui_file_filter_t;

// open_file / save_file flags.
//
// MULTISELECT is open-only (save_file ignores it - a save dialog has
// exactly one destination). DIRECTORY makes the dialog pick an existing
// folder rather than a file; it is open-only, and the filter list is
// ignored while it is set. NO_OVERWRITE_PROMPT is save-only and turns
// OFF the default "file exists, replace it?" confirmation. SHOW_HIDDEN
// asks for dot-files to be listed; native dialogs may ignore it (the
// user's own shell setting wins on macOS and Windows), so treat it as a
// hint rather than a guarantee.
#define NEUI_FD_MULTISELECT          0x0001u
#define NEUI_FD_DIRECTORY            0x0002u
#define NEUI_FD_NO_OVERWRITE_PROMPT  0x0004u
#define NEUI_FD_SHOW_HIDDEN          0x0008u

// Dialog description. Every pointer field may be NULL, in which case the
// host default applies. Zero-initialise and set only what you need:
//
//   neui_file_filter_t f[] = { { "Presets", "*.preset" }, { "All files", "*" } };
//   neui_file_dialog_t d = {0};
//   d.title = "Load preset"; d.filters = f; d.filter_count = 2;
//
// `initial_name` is the pre-filled file name (save_file) and is ignored
// by open_file. `default_filter` indexes `filters`; out-of-range values
// clamp to 0.
typedef struct neui_file_dialog
{
  const char* title;               // NULL = host default caption
  const char* initial_dir;         // NULL = host default / last used
  const char* initial_name;        // save_file only
  const neui_file_filter_t* filters;
  uint32_t    filter_count;
  uint32_t    default_filter;      // index into filters
  uint32_t    flags;               // NEUI_FD_* combination
} neui_file_dialog_t;

// Per-path result callback. Invoked once per chosen path, in the order
// the dialog reports them, BEFORE open_file / save_file returns. `path`
// is an absolute UTF-8 filesystem path owned by the framework and valid
// only for the duration of the call - copy it if you need to keep it.
typedef void (NEUI_ABI *neui_file_path_cb)(void* userdata, const char* path);

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

  /* ---- appended in a later version: null-check before calling ---- */

  // Run a modal "open" dialog owned by `parent_window` (APPWINDOW,
  // PLUGWINDOW, or DIALOG). Blocks the calling thread until the user
  // confirms or cancels - the same modal contract as message_box - then
  // invokes `cb` once per chosen path before returning.
  //
  // Returns the number of paths delivered: 0 = the user cancelled, N>0 =
  // that many callbacks fired, -1 = the dialog could not be shown at all
  // (bad widget, cross-session handle, host without a file-dialog
  // surface). Distinguishing -1 from 0 matters: a client that wants to
  // offer its own path entry as a fallback should do so only on -1.
  // Passing a NULL `cb` still runs the dialog and still returns the
  // count, which is a cheap way to ask "does this host have a file
  // dialog?" - but it also means the user's choice is discarded, so
  // prefer feature-detecting on the slot being non-NULL.
  //
  // Per host: win32 = IFileDialog (IFileOpenDialog); macOS = NSOpenPanel
  // run modally; Linux = the XDG desktop portal when D-Bus and a portal
  // implementation are both present, otherwise a neui-drawn browser over
  // the Cairo backend (same graceful degradation as the message box, and
  // the portal falls through to it on any error). The null platform and
  // iOS return -1.
  int  (NEUI_ABI *open_file)(neui_session_t session,
                             neui_widget_t parent_window,
                             const neui_file_dialog_t* desc,
                             neui_file_path_cb cb, void* userdata);

  // Run a modal "save" dialog. Same contract and return values as
  // open_file, except that at most one path is ever delivered (so the
  // return is 0 or 1, or -1 for unsupported) and the path names a file
  // that may not exist yet - the client creates it.
  //
  // The chosen name is completed against the active filter when the user
  // typed no extension: picking "Presets (*.preset)" and typing "lead"
  // yields ".../lead.preset". A name that already carries an extension
  // is left alone, even if it does not match the filter.
  //
  // Per host: win32 = IFileSaveDialog; macOS = NSSavePanel; Linux =
  // portal SaveFile or the neui-drawn browser. Overwrite confirmation is
  // on by default (NEUI_FD_NO_OVERWRITE_PROMPT turns it off).
  int  (NEUI_ABI *save_file)(neui_session_t session,
                             neui_widget_t parent_window,
                             const neui_file_dialog_t* desc,
                             neui_file_path_cb cb, void* userdata);
} neui_notify_api_t;

#ifdef __cplusplus
}
#endif
