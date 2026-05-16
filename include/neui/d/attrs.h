#pragma once

#include <stdint.h>
#include "api.h"
#include "widgets.h"

#ifdef __cplusplus
extern "C" {
#endif

// Generic per-widget attribute API.
//
// Attributes are string-keyed name/value pairs stored on a widget. They unify
// implicit widget variants (e.g. CHECKBOX3 == CHECKBOX + tristate=1) and let
// clients configure orthogonal features (password, readonly, …) without
// growing the core widget API.
//
// Attributes that change observable behavior (tristate, multiline, password)
// SHOULD be set before widget_show(); hosts MAY ignore post-show changes when
// the underlying native control cannot be reconfigured in place.
//
// Unknown keys are stored and readable but have no behavioral effect; this
// leaves room for forward-compatible client code.
//
// Key namespaces:
//   neui.attr.*   - platform-neutral, stable across hosts
//   neui.win32.*  - reserved, host-specific (Win32)
//   neui.macos.*  - reserved, host-specific (macOS)
//   neui.linux.*  - reserved, host-specific (Linux)

#define NEUI_API_ATTRS "com.defiantnerd.neui.extension.attrs/0"

// ---- Well-known platform-neutral keys -------------------------------------

// int (bool): cycles unchecked → checked → indeterminate on a CHECKBOX.
// Implicit on CHECKBOX3.
#define NEUI_ATTR_TRISTATE  "neui.attr.tristate"

// int (bool): multi-line text editor semantics on INPUTBOX.
// Implicit on MULTILINE.
#define NEUI_ATTR_MULTILINE "neui.attr.multiline"

// int (bool): text content cannot be modified by the user (INPUTBOX, MULTILINE).
#define NEUI_ATTR_READONLY  "neui.attr.readonly"

// int (bool): masks input characters on INPUTBOX.
#define NEUI_ATTR_PASSWORD  "neui.attr.password"

// int (bool): participates in TAB / SHIFT+TAB traversal.
// Supersedes neui_widget_api_t::set_tab_stop.
#define NEUI_ATTR_TAB_STOP  "neui.attr.tab_stop"

// string: "left", "center", "right" (LABEL, BUTTON, SECTION).
// On SECTION this selects the horizontal position of the optional header
// label drawn in the band at the top of the section rectangle. Default
// for SECTION is "center" (when the attribute is unset).
#define NEUI_ATTR_ALIGN_TEXT "neui.attr.align_text"

// int (logical pixels): minimum/maximum drag-resize bounds for a frame
// window (APPWINDOW / PLUGWINDOW). Unset, 0, or negative means no constraint.
// max < min is treated as "no maximum". Hosts also clamp programmatic
// set_size calls to the same range.
#define NEUI_ATTR_MIN_WIDTH  "neui.attr.min_width"
#define NEUI_ATTR_MIN_HEIGHT "neui.attr.min_height"
#define NEUI_ATTR_MAX_WIDTH  "neui.attr.max_width"
#define NEUI_ATTR_MAX_HEIGHT "neui.attr.max_height"

// string: file path to an icon image for a frame window (APPWINDOW).
// Accepted formats: .ico (preferred - multi-resolution), .png, .bmp, .jpg.
// Empty string clears the icon back to the default. Set before show() for
// initial paint, or any time after for a runtime change.
#define NEUI_ATTR_ICON_PATH  "neui.attr.icon_path"

// float (radians): rotation applied around the centre of the widget.
// Currently honoured by IMAGE; can be extended to other painted widgets
// via the renderer transform stack. Positive values rotate clockwise on
// screen (Y axis is screen-down). 0 / unset = no rotation.
#define NEUI_ATTR_ROTATION "neui.attr.rotation"

// int (ARGB): background colour for a self-painted widget (KNOB, IMAGE,
// future painted controls). 0xAARRGGBB. When unset, the host uses
// (a) the system-theme palette if the parent frame's
// NEUI_ATTR_FOLLOW_SYSTEM_THEME is set, otherwise (b) the OS window
// background colour (COLOR_WINDOW on Win32). Honoured unconditionally -
// independent of NEUI_ATTR_FOLLOW_SYSTEM_THEME - so clients can recolour
// individual painted panels without buying into theme tracking.
#define NEUI_ATTR_BACKGROUND  "neui.attr.background"

// int: per-session theme override. Default 0 (AUTO) means follow the
// OS light/dark setting. 1 (LIGHT) and 2 (DARK) force the corresponding
// palette regardless of the OS preference. The system accent colour
// stays live in all three modes (only the surface + text family is
// forced). Set via attrs->set_session_int (not per-widget). Live -
// changing it triggers a full repaint.
//
//   NEUI_THEME_MODE_AUTO  = 0   (default - follow OS)
//   NEUI_THEME_MODE_LIGHT = 1
//   NEUI_THEME_MODE_DARK  = 2
#define NEUI_ATTR_THEME_MODE "neui.attr.theme_mode"

// int (bool): per-frame opt-in (APPWINDOW / PLUGWINDOW / DIALOG) for
// system theme tracking. Honoured on both the win32 native host and the
// crossplatform (self-painted) host.
//   0 / unset (default) - host preserves OS-default chrome: native
//                          controls render OS-default style; painted
//                          widgets clear to COLOR_WINDOW; title bar
//                          stays the system default; theme changes do
//                          not invalidate the frame.
//   1                   - host applies the shared theme palette: title
//                          bar via DWMWA_USE_IMMERSIVE_DARK_MODE, dark
//                          mode on the HMENU strip, and on the win32
//                          host the WM_CTLCOLOR* handlers + custom-
//                          draw for ListBox / TreeView. Live-reapplied
//                          when the system theme changes (the frame
//                          invalidates on every system flip).
// Opt-in exists so frames stay backwards compatible for clients
// (notably DAW-hosted audio plugins) where the host application owns
// the look-and-feel and the OS theme is irrelevant.
#define NEUI_ATTR_FOLLOW_SYSTEM_THEME "neui.attr.follow_system_theme"

// string: "horizontal" (default) or "vertical". Applies to SLIDER. Read at
// widget_show; not live-updateable in v1 (would require recreation).
#define NEUI_ATTR_ORIENTATION "neui.attr.orientation"

// string: anchor end of the knob's active fill arc.
//   "min"    - arc fills from sweep start (7 o'clock) toward the current value (default)
//   "center" - arc fills from 12 o'clock toward the current value (bipolar params)
//   "max"    - arc fills from sweep end (5 o'clock) backward to the current value
// Applies to KNOB. Live-readable each paint.
#define NEUI_ATTR_POLARITY "neui.attr.polarity"

// int: number of distinct positions the value can take. Applies to SLIDER
// and KNOB. Values below 2 are treated as "continuous" (no snapping, no
// extra tick marks beyond the existing endpoint markers). When >= 2:
//   - User-driven changes (drag/click/wheel/keys) snap to the nearest
//     of N evenly-spaced positions on [0..1].
//   - Programmatic attrs->set_float(NEUI_PARAM_VALUE, ...) also snaps.
//   - Tick marks are drawn at every step position.
#define NEUI_ATTR_STEPS "neui.attr.steps"

// int (bool): whether a NEUI_W_DIALOG frame blocks input on its owner.
//   1 (or unset) - modal: owner is disabled while the dialog is shown,
//                  re-enabled and re-activated when the dialog is destroyed.
//                  This is the historical behaviour and remains the default.
//   0           - modeless: owner stays interactive; dialog is just a
//                  separate frame that happens to track an owner for
//                  z-order / parent-relative placement.
// Read once at widget_show; not live-updateable.
#define NEUI_ATTR_MODAL "neui.attr.modal"

// string: optional value-text overlay rendered inside the KNOB widget,
// horizontally centred below the disc. Empty / unset = no overlay.
// The framework reads this attribute each paint; the client typically
// updates it from a NEUI_EVENT_WIDGET_PREUPDATE handler so the overlay
// reflects the latest value just before the disc is drawn.
#define NEUI_ATTR_VALUE_TEXT "neui.attr.value_text"

// ---- Live parameter slots (drive rendering of SLIDER, KNOB, future
// value-bearing widgets). Float, normalized to [0..1]; out-of-range
// values are clamped on set. Programmatic set does NOT fire
// NEUI_EVENT_VALUE_CHANGED (only user manipulation does).

// float [0..1]: current normalized value of a value-bearing widget.
#define NEUI_PARAM_VALUE   "neui.param.value"

// float [0..1]: default value used by double-click reset on KNOB. If absent,
// reset goes to 0.
#define NEUI_PARAM_DEFAULT "neui.param.default"

// Values for NEUI_ATTR_THEME_MODE.
enum {
  NEUI_THEME_MODE_AUTO  = 0,   // follow the OS light/dark setting
  NEUI_THEME_MODE_LIGHT = 1,   // force light palette
  NEUI_THEME_MODE_DARK  = 2,   // force dark palette
};

typedef struct neui_attr_api {
  uint32_t neui_version;

  // Integer (bool / enum) attributes.
  // Returns 1 on success, 0 on failure (bad widget, bad key).
  int         (NEUI_ABI *set_int)    (neui_session_t session, neui_widget_t widget,
                                      const char* key, int32_t value);
  // Returns the stored value, or default_value if the key is absent.
  int32_t     (NEUI_ABI *get_int)    (neui_session_t session, neui_widget_t widget,
                                      const char* key, int32_t default_value);

  // String attributes. set_string copies the value; get_string returns a
  // pointer owned by the host (valid until the next set_string / remove /
  // widget destroy).
  int         (NEUI_ABI *set_string) (neui_session_t session, neui_widget_t widget,
                                      const char* key, const char* value);
  const char* (NEUI_ABI *get_string) (neui_session_t session, neui_widget_t widget,
                                      const char* key);

  // Returns 1 if the key is present on the widget, 0 otherwise.
  int         (NEUI_ABI *has)        (neui_session_t session, neui_widget_t widget,
                                      const char* key);

  // Removes the key. Returns 1 if the key was present, 0 otherwise.
  // For behavioral keys already reflected in native state, hosts SHOULD treat
  // remove() as "revert to default"; when that is not possible (e.g. a
  // native control already created with a fixed style bit) the removal is
  // recorded but the observable behavior MAY persist until the widget is
  // re-created.
  int         (NEUI_ABI *remove)     (neui_session_t session, neui_widget_t widget,
                                      const char* key);

  // Float attributes. Stored as IEEE-754 single precision; intended for
  // configuration values (opacity, ratios, durations) where double-precision
  // range and precision are not required.
  // set_float returns 1 on success, 0 on failure.
  int         (NEUI_ABI *set_float)  (neui_session_t session, neui_widget_t widget,
                                      const char* key, float value);
  // Returns the stored value, or default_value if the key is absent or the
  // stored value has a different type (no implicit coercion from int / string).
  float       (NEUI_ABI *get_float)  (neui_session_t session, neui_widget_t widget,
                                      const char* key, float default_value);

  // ---- Session-level attributes ---------------------------------------
  // Same key/value semantics as the per-widget variants above, but stored
  // on the session itself (no widget). Used for session-wide settings
  // like NEUI_ATTR_THEME_MODE. Reserved for future session-wide knobs;
  // unknown keys are stored and retrievable but have no behaviour.
  int     (NEUI_ABI *set_session_int)(neui_session_t session,
                                       const char* key, int32_t value);
  int32_t (NEUI_ABI *get_session_int)(neui_session_t session,
                                       const char* key, int32_t default_value);
} neui_attr_api_t;

#ifdef __cplusplus
}
#endif
