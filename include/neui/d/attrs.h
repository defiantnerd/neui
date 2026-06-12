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
// clients configure orthogonal features (password, readonly, ...) without
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

// int (bool): cycles unchecked -> checked -> indeterminate on a CHECKBOX.
// Implicit on CHECKBOX3.
#define NEUI_ATTR_TRISTATE  "neui.attr.tristate"

// int (bool): read-only marker, set by the host when a NEUI_W_MULTILINE
// widget is created. Multi-line vs single-line is determined by the widget
// TYPE at creation (NEUI_W_MULTILINE vs NEUI_W_INPUTBOX), NOT by this
// attribute: the hosts never read it, so setting it on a live NEUI_W_INPUTBOX
// does not turn it into a multi-line editor. Present so clients can query
// which variant they got via attrs->get_int.
#define NEUI_ATTR_MULTILINE "neui.attr.multiline"

// int (bool): soft word-wrap for a MULTILINE widget.
//   0 / unset (default) - no wrap: each logical line (text between '\n's)
//                          renders as one row; a long line runs past the
//                          right edge and is clipped (horizontal scrolling
//                          is not provided).
//   1                   - wrap: long logical lines are broken into multiple
//                          visual rows at the widget's content width,
//                          preferring word boundaries (falling back to a
//                          character break for a word wider than the line).
//                          Explicit '\n's still force a new row. Vertical
//                          scrolling / navigation / selection operate on the
//                          resulting visual rows. Live - toggling re-flows on
//                          the next paint. Honoured by the crossplatform host
//                          today; the native hosts use their control's own
//                          wrap behaviour.
#define NEUI_ATTR_LINE_WRAP "neui.attr.line_wrap"

// int (bool): text content cannot be modified by the user (INPUTBOX, MULTILINE).
#define NEUI_ATTR_READONLY  "neui.attr.readonly"

// int (bool): masks input characters on INPUTBOX.
#define NEUI_ATTR_PASSWORD  "neui.attr.password"

// int (bool): participates in TAB / SHIFT+TAB traversal.
// Supersedes neui_widget_api_t::set_tab_stop.
#define NEUI_ATTR_TAB_STOP  "neui.attr.tab_stop"

// string: "none", "left", "center", "right" (LABEL, BUTTON, SECTION).
// On SECTION this selects the horizontal position of the optional header
// label drawn in the band at the top of the section rectangle. "none"
// hides the header band entirely so the body fills the whole rect
// (same path as an empty text). Default is "center" (when the
// attribute is unset).
#define NEUI_ATTR_ALIGN_TEXT "neui.attr.align_text"

// string: scroll axis configuration for a SECTION container.
//   "none"       - non-scrolling (default; existing behaviour).
//   "vertical"   - vertical scrollbar on the right edge of the body.
//   "horizontal" - horizontal scrollbar on the bottom edge of the body.
//   "both"       - dual scrollbars; bottom-right corner is a dead square.
// When non-"none" the SECTION clips its children to the body rect and
// translates them by the current scroll offset. Mouse wheel inside the
// section body scrolls; Shift+wheel scrolls horizontally on the "both"
// and "horizontal" modes. Read at widget_show; live-updateable.
#define NEUI_ATTR_SCROLL_MODE "neui.attr.scroll_mode"

// int (logical px): explicit content extent for a scrolling SECTION.
// 0 / unset = auto: the SECTION scans its direct children and takes
// max(child.x + child.width) / max(child.y + child.height) as the
// content size. Setting an explicit value overrides the auto-bound
// calculation - useful when the SECTION's children are still being
// added incrementally or when the scrollable region intentionally
// extends past the laid-out content. Live - the next paint re-clamps.
#define NEUI_ATTR_CONTENT_WIDTH  "neui.attr.content_width"
#define NEUI_ATTR_CONTENT_HEIGHT "neui.attr.content_height"

// int (neui_scroll_kinetics_t): wheel kinetics selector for scrollable
// widgets (SECTION + GRID). Generic alternative to the GRID-only
// NEUI_ATTR_GRID_SCROLL_MODE; numeric values are intentionally identical
// so existing GRID callers can adopt either key.
//   NEUI_SCROLL_KINETICS_PLATFORM (0, default) - host picks the natural
//                                                 feel. macOS: SMOOTH;
//                                                 Win32 / null: STEPPED.
//   NEUI_SCROLL_KINETICS_STEPPED  (1) - hard-clamp at top / bottom, no
//                                       momentum, no rubber-band.
//   NEUI_SCROLL_KINETICS_SMOOTH   (2) - pixel-precise motion with
//                                       WebKit-style elastic overscroll
//                                       and a 60 Hz spring-back to the
//                                       boundary.
// Live - the next wheel tick uses the new mode. On GRID, this attr takes
// precedence over NEUI_ATTR_GRID_SCROLL_MODE when both are set; the GRID
// attr remains as a back-compat alias.
#define NEUI_ATTR_SCROLL_KINETICS "neui.attr.scroll_kinetics"

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

// string: font family for text rendered by the widget (e.g. "Consolas",
// "Segoe UI"). Empty / unset leaves the host's system default in place
// (Segoe UI on the Win32 / D2D backend). Honoured by the Direct2D
// backend and the two hosts that use it (native win32 + crossplatform
// on Windows); cg / null backends ignore it (text continues to render
// in the system default). Live - applies on the next paint for painted
// widgets, and via WM_SETFONT on the native win32 host.
#define NEUI_ATTR_FONT_FAMILY "neui.attr.font_family"

// float (logical pixels at 96 DPI): font size for text rendered by the
// widget. Unset / <= 0 keeps the widget's existing hardcoded default
// (typically 12 px). Honoured everywhere draw_text / measure_text
// flows through the widget's paint code, including the MULTILINE /
// INPUTBOX caret + IME composition geometry so positioning stays
// consistent with the rendered glyphs.
#define NEUI_ATTR_FONT_SIZE "neui.attr.font_size"

// int: font weight in CSS-style 100..900. 400 = Normal, 700 = Bold;
// 0 / unset = Normal. Honoured by the Direct2D backend (mapped to the
// nearest DWRITE_FONT_WEIGHT_*) and the native win32 host (mapped to
// LOGFONTW::lfWeight). Italic is not exposed in v1; the style stays
// Normal regardless of weight.
#define NEUI_ATTR_FONT_WEIGHT "neui.attr.font_weight"

// int: maximum number of item rows shown in a COMBOBOX drop list before it
// scrolls. Applies to COMBOBOX. The drop list grows to fit the item count
// up to this cap; beyond it a scrollbar appears. Values below 1 are clamped
// to 1. Unset = 10. The client supplies only the *collapsed* combobox
// rectangle at create time (x / y / width / height); the drop list is sized
// independently from this attribute + the item count, so the client no
// longer reserves drop-list space in the widget height.
//   xpl host: fully honoured (self-painted overlay).
//   win32 native: honoured (CB_SETMINVISIBLE + window-height bump).
//   macOS native: best-effort - NSPopUpButton auto-sizes its menu and has no
//                 visible-row cap, so this attribute is ignored there.
#define NEUI_ATTR_COMBO_MAX_VISIBLE "neui.attr.combo_max_visible"

// int (logical px): explicit width override for a COMBOBOX drop list.
// Applies to COMBOBOX. 0 / unset = auto: the drop list is sized to the widest
// entry (plus padding and a scrollbar column when the list overflows). When
// set, the drop list uses this width. In both cases the drop list is never
// narrower than the collapsed combobox width.
//   xpl host: fully honoured.
//   win32 native: honoured (CB_SETDROPPEDWIDTH).
//   macOS native: ignored (NSPopUpButton auto-sizes its menu width).
#define NEUI_ATTR_COMBO_DROP_WIDTH "neui.attr.combo_drop_width"

// int: drag interaction style of the KNOB widget.
//   NEUI_KNOB_MODE_ROTATIONAL = 0 (default) - cursor angle around the
//                                              knob centre drives the value
//                                              (classic rotary feel).
//   NEUI_KNOB_MODE_VERTICAL   = 1            - vertical drag; up increases.
//   NEUI_KNOB_MODE_HORIZONTAL = 2            - horizontal drag; right increases.
// Cached on mouse-down for the duration of the drag, so reading is O(1)
// per WM_MOUSEMOVE; changes mid-drag apply on the next drag.
#define NEUI_ATTR_KNOB_MODE "neui.attr.knob_mode"

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

// Values for NEUI_ATTR_SCROLL_KINETICS. Numerically identical to
// neui_grid_scroll_mode_t so the two attrs can be treated as aliases.
typedef enum neui_scroll_kinetics {
  NEUI_SCROLL_KINETICS_PLATFORM = 0,   // host picks (macOS=smooth, Win32=stepped)
  NEUI_SCROLL_KINETICS_STEPPED  = 1,   // hard-clamp, no rubber-band
  NEUI_SCROLL_KINETICS_SMOOTH   = 2,   // elastic + spring-back
} neui_scroll_kinetics_t;

// Values for NEUI_ATTR_KNOB_MODE.
enum {
  NEUI_KNOB_MODE_ROTATIONAL = 0,   // cursor angle around the centre (default)
  NEUI_KNOB_MODE_VERTICAL   = 1,   // vertical drag, up increases
  NEUI_KNOB_MODE_HORIZONTAL = 2,   // horizontal drag, right increases
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
