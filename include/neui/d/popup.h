#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"
#include "events.h"   // neui_widget_t

#ifdef __cplusplus
extern "C" {
#endif

// Popup surfaces - a frameless overlay that may leave its owner frame.
//
// THE PROBLEM. A popup painted into the owner frame's surface cannot be larger
// than that frame, because there is nowhere else for the pixels to go. For a
// desktop app that is fine. For an audio-plugin editor it is a hard ceiling:
// preset browsers, modulation-routing pickers and FX selectors routinely need
// more room than the editor window has (a three-column FX grid is ~1030x970
// logical px, opened from an editor considerably smaller than that), and every
// commercial plugin serves them by putting the menu on the desktop as its own
// top-level window hanging over the DAW.
//
// A NEUI_W_POPUPSURFACE is that primitive, generalized: not a menu, but an
// overlay you fill with ordinary neui widgets. Menus, drop-lists, tooltips,
// browsers and pickers are all the same shape and all hit the same ceiling.
//
// WHAT IT IS. A popup surface is a FRAME - the third kind, after APPWINDOW and
// PLUGWINDOW. It has its own coordinate origin, its own render surface, and its
// children are laid out against it exactly as a window's children are:
//
//   neui_widget_t pop = widgets->create(sess, widget_none, NEUI_W_POPUPSURFACE,
//                                       0, 0, 420, 560, NULL);
//   widgets->create(sess, pop, NEUI_W_CUSTOMDRAW, 0, 0, 420, 560, NULL);
//   popup->open(sess, pop, anchor_button, 0, 0, NEUI_POPUP_BELOW);
//
// Note it is created with parent = widget_none, like any other frame. `open`
// establishes the relationship to the anchor; a popup is not a child widget of
// the thing it hangs off.
//
// TWO BACKINGS, ONE API. Where the platform has owned top-level windows
// (win32 / macOS / X11) the host backs the surface with a real, borderless,
// non-activating window owned by the anchor's frame - so it may extend past the
// editor and over the DAW. Where the platform cannot (iOS, WASM/canvas, LVGL,
// null) the host backs it with an in-frame overlay clamped to the owner. The
// client authors the content once and it paints identically either way.
//
// So do NOT branch on "is this a desktop window". Ask get_clamp_size() what box
// you will be given and lay out against that - see below.
//
// BEST-EFFORT, EVEN WHERE SUPPORTED. A DAW that runs plugin UIs out of process
// or sandboxed may not z-order our window against its own, and full-screen
// exclusive modes break it outright. There is no reliable way to detect this,
// so treat desktop placement as very-likely rather than guaranteed.
//
// LIFETIME. Dismissal is the host's job, and it happens on: a press anywhere
// outside the open stack, the owner being deactivated or another application
// coming forward, the owner being moved / resized / hidden / destroyed, and
// Escape. Each dismissal reports NEUI_EVENT_POPUP_DISMISSED so a client can
// drop its own state. A dismissal cannot be vetoed - a popup that could refuse
// to close is a window stuck over someone's DAW.
//
// The press that dismisses is SWALLOWED: it closes the popup and does not also
// act on the widget underneath. This matches every OS menu and drop-list, and
// it is what stops one click from both closing a picker and moving a parameter.
//
// KEYS REACH THE POPUP, BUT NOTHING INTERPRETS THEM FOR YOU. The surface
// deliberately never takes activation (a DAW must not see its editor lose
// focus), so keys arrive at the OWNER's window - the host then retargets them:
//
//   - Escape closes the whole stack (reported as NEUI_POPUP_DISMISS_ESCAPE).
//   - With focus INSIDE the deepest open surface, keys route to the focused
//     widget exactly as they would in any frame. A NEUI_W_INPUTBOX inside a
//     popup is focusable by click, and typing into it works.
//   - With focus anywhere else, NEUI_EVENT_KEYDOWN / _KEYUP / _KEYCHAR are
//     delivered with `.widget` set to the deepest SURFACE itself, so a
//     client-drawn menu or browser can implement its own navigation.
//
// In every case the key is SWALLOWED - it never reaches the widgets under an
// open popup, the same rule the dismissing press follows.
//
// What the host does NOT do: interpret arrows / Home / End / type-ahead over
// your content, focus the popup when it opens (call widgets->set_focus if you
// want that), or move focus into a popup from the owner. Tab cycles the popup's
// own tab stops once focus is already inside it. Press-drag-release from the
// anchor into the popup is not supported either - click, then click.

#define NEUI_API_POPUP "com.defiantnerd.neui.extension.popup/0"

// Where the surface is placed relative to the anchor rectangle. Every mode
// FLIPS to the opposite side when the popup would not fit in the clamp box on
// the preferred side but does fit on the other, then clamps if neither fits.
typedef enum neui_popup_side
{
  // Below the anchor, left edges aligned. The drop-list / menu-bar default.
  NEUI_POPUP_BELOW = 0,
  // Above the anchor, left edges aligned.
  NEUI_POPUP_ABOVE = 1,
  // To the right of the anchor, top edges aligned. The submenu cascade
  // direction - a second level opens right of the first and flips left at the
  // screen edge.
  NEUI_POPUP_RIGHT = 2,
  // To the left of the anchor, top edges aligned.
  NEUI_POPUP_LEFT  = 3,
  // At the pointer, ignoring the anchor rectangle: the (x, y) passed to open()
  // is still honoured as an offset from the pointer. The context-menu mode.
  NEUI_POPUP_AT_POINTER = 4,
} neui_popup_side_t;

// Why a surface closed (neui_event_popup_t::reason).
typedef enum neui_popup_dismiss_reason
{
  NEUI_POPUP_DISMISS_OUTSIDE_PRESS = 0,  // press landed outside the open stack
  NEUI_POPUP_DISMISS_DEACTIVATED   = 1,  // owner lost activation / app switched
  NEUI_POPUP_DISMISS_OWNER_MOVED   = 2,  // owner moved, resized, hid or died
  NEUI_POPUP_DISMISS_ESCAPE        = 3,  // Escape pressed
  NEUI_POPUP_DISMISS_CLIENT        = 4,  // the client called close / close_all
  NEUI_POPUP_DISMISS_CASCADE       = 5,  // a level above this one closed
} neui_popup_dismiss_reason_t;

typedef struct neui_popup_api
{
  uint32_t neui_version;

  // Show `surface` (a NEUI_W_POPUPSURFACE) anchored to `anchor`. (x, y) are an
  // offset in the ANCHOR's local logical pixels - (0, 0) with NEUI_POPUP_BELOW
  // puts the popup's top-left at the anchor's bottom-left. With
  // NEUI_POPUP_AT_POINTER the offset is from the current pointer position
  // instead. `side` is a neui_popup_side_t.
  //
  // There is deliberately no screen-coordinate form: neui's coordinate contract
  // is logical px at 96 DPI and NEUI_ATTR_UI_SCALE is built on it, so absolute
  // screen placement would introduce a second coordinate space that multi-
  // monitor DPI then makes ambiguous. Anchor + offset + side covers the cases
  // (a drop-list wants BELOW, a context menu wants AT_POINTER, a submenu wants
  // RIGHT off its parent row - anchor that one to the level above).
  //
  // The surface inherits the owner frame's NEUI_ATTR_UI_SCALE, so a zoomed
  // editor does not sprout a 100 % popup.
  //
  // Opening a surface that is already open re-places it. Opening a SECOND
  // surface while one is up pushes a cascade level: it is dismissed with its
  // parent, and a press on the parent level closes just the deeper levels.
  //
  // Returns false (showing nothing) for a non-POPUPSURFACE widget, a bad or
  // cross-session handle, an anchor with no frame, or a zero-sized surface.
  bool (NEUI_ABI *open)(neui_session_t session, neui_widget_t surface,
                        neui_widget_t anchor, int x, int y, int side);

  // Close `surface` and every level opened above it. No-op if it is not open.
  // Reports NEUI_EVENT_POPUP_DISMISSED with reason CLIENT (and CASCADE for the
  // levels above).
  void (NEUI_ABI *close)(neui_session_t session, neui_widget_t surface);

  // Close every open popup surface in the session.
  void (NEUI_ABI *close_all)(neui_session_t session);

  // True while `surface` is open.
  bool (NEUI_ABI *is_open)(neui_session_t session, neui_widget_t surface);

  // The size of the box a popup anchored to `anchor` will be clamped to, in
  // logical px. Call this BEFORE laying content out: it is the one fact a
  // client needs at design time, and it is a box rather than a capability bit
  // on purpose. A 1030x970 FX grid that has to live inside a 700x500 editor is
  // not a smaller grid, it is a scrolling browser - and that is a content
  // decision, not a placement one.
  //
  // With a desktop backing this is the work area of the monitor the anchor's
  // frame is on (which can still be smaller than a big popup - desktop
  // placement buys a much larger box, not an unbounded one). With an in-frame
  // backing it is the owner frame's client area.
  //
  // The recommended idiom is to put popup content in a scrolling
  // NEUI_W_SECTION sized to min(content, clamp): the same client code is then
  // a full-size grid on the desktop and a scrolling one where it must fit
  // inside the editor, with no second UI to write.
  //
  // Out-pointers may be NULL. Reports (0, 0) for a bad anchor.
  void (NEUI_ABI *get_clamp_size)(neui_session_t session, neui_widget_t anchor,
                                  int* width, int* height);

  // True when a popup anchored to `anchor` gets its own OS window and may
  // therefore extend past the owner frame; false when it will be clamped into
  // the frame. Prefer get_clamp_size for layout - this is for a client that
  // wants to report or log the placement it got, not for forking a UI.
  bool (NEUI_ABI *escapes_frame)(neui_session_t session, neui_widget_t anchor);
} neui_popup_api_t;

#ifdef __cplusplus
}
#endif
