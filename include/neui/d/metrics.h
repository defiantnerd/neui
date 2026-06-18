#pragma once

#include <stdint.h>
#include "api.h"
#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

// Platform layout metrics, so clients can lay out responsively instead of
// hard-coding pixel sizes that look wrong when the platform's text/control
// scale differs from the desktop default. This matters most on iOS, where
// Dynamic Type grows the system text size live: a client that queries these
// metrics (and re-runs its layout on NEUI_EVENT_METRICS_CHANGED) follows the
// user's accessibility text-size setting without per-platform code.
//
// Coordinate system: every value returned here is in LOGICAL pixels at 96 DPI
// (the same convention as widget x/y/width/height and the rest of the API), so
// a client can use them directly in widgets->create / set_pos / set_size.
//
// Scope: iOS-first. On iOS the values are real (driven by the live Dynamic
// Type scale + UISwitch size + the view's safeAreaInsets). On the desktop
// hosts the values are sensible defaults (scale 1.0 => the framework's own
// default control/row/font sizes); measure_text is iOS-accurate and a
// best-effort estimate on desktop (see the per-method notes). A host that
// has no metrics story simply does not expose this interface; clients
// feature-detect via get_interface returning NULL.
#define NEUI_API_METRICS "neui.api.metrics/0"

// Standard layout metrics. All return logical pixels (96 DPI).
//
// The size metrics scale with the host's painted-UI scale: 1.0 on the desktop
// hosts (so they equal the framework's default control/row/etc. sizes) and the
// Dynamic-Type-derived scale on iOS (so they grow with the user's text-size
// setting). FONT_SIZE metrics are the painted default text size times that
// same scale. SWITCH_WIDTH / SWITCH_HEIGHT are the native toggle size: the iOS
// UISwitch (51x31) on iOS, a sensible desktop value otherwise.
typedef enum neui_metric_t
{
  NEUI_METRIC_CONTROL_HEIGHT,     // standard button / input / control height
  NEUI_METRIC_ROW_HEIGHT,         // list / grid body row height
  NEUI_METRIC_BODY_FONT_SIZE,     // default body text size
  NEUI_METRIC_LABEL_FONT_SIZE,    // default label / caption text size
  NEUI_METRIC_MARGIN,             // outer margin around a content region
  NEUI_METRIC_SPACING,            // gap between adjacent controls
  NEUI_METRIC_CHECKBOX_SIZE,      // checkbox / radio box edge length
  NEUI_METRIC_SWITCH_WIDTH,       // toggle-switch width  (iOS UISwitch on iOS)
  NEUI_METRIC_SWITCH_HEIGHT,      // toggle-switch height (iOS UISwitch on iOS)
  NEUI_METRIC_SCROLLBAR_THICKNESS // scrollbar track thickness
} neui_metric_t;

// A note on the `session` parameter every method below carries: the UI scale
// and the layout metrics are PROCESS-GLOBAL (the painted-UI scale is a single
// process-wide value - the same class of limitation as the process-wide theme
// palette), so `session` is accepted for signature consistency with the rest
// of the API (and future per-session use) but is not consulted by ui_scale /
// metric / measure_text - passing any value, including an unknown one, returns
// the live global figure. Only safe_area_insets actually reads its arguments
// (the `frame` widget's view). Acquire the vtable per-session as usual via
// get_interface(session, NEUI_API_METRICS).
typedef struct neui_metrics_api
{
  uint32_t neui_version;

  // The painted / text UI scale: 1.0 on the desktop hosts, the live Dynamic
  // Type scale on iOS (preferred body point-size / 12). Every size + font
  // metric above is derived from this, so a client that only wants to scale
  // its own custom layout can read this single number. Process-global (see the
  // note above); `session` is not consulted.
  float (NEUI_ABI *ui_scale)(neui_session_t session);

  // A standard layout metric in logical pixels. Process-global; `session` is
  // not consulted (see the note above). An unrecognized metric returns 0.
  int (NEUI_ABI *metric)(neui_session_t session, neui_metric_t metric);

  // Width of `text` in logical pixels, usable OUTSIDE a paint callback (no
  // live render context required) - e.g. to size a button to its label while
  // laying out. `family` NULL/empty = host default; `size_px` <= 0 = the body
  // font size (NEUI_METRIC_BODY_FONT_SIZE); `weight` 0 = normal (CSS 400),
  // else a CSS weight 100..900. iOS-accurate (measured via UIFont /
  // -sizeWithAttributes:); on the desktop hosts this is a best-effort estimate
  // (a font-metric-based average advance) - good enough for layout sizing but
  // not pixel-exact. `session` is not consulted (see the note above). Returns 0
  // for NULL/empty text.
  int (NEUI_ABI *measure_text)(neui_session_t session, const char* text,
                               const char* family, float size_px, int weight);

  // The frame's safe-area insets in logical pixels. Complements
  // get_client_rect: where get_client_rect gives the usable content RECT
  // (already excluding the safe area + any host menubar band), this exposes
  // the raw inset on each edge so a client can, e.g., extend a background to
  // the screen edge while keeping its controls inside the safe area. `frame`
  // must be an APPWINDOW / PLUGWINDOW / DIALOG. iOS-real (the frame view's
  // safeAreaInsets, with the hamburger menu band folded into `top` to stay
  // consistent with get_client_rect); the desktop hosts report 0 on every
  // edge (sensible default - desktop chrome lives outside the client area).
  // Any out pointer may be NULL to skip that edge.
  void (NEUI_ABI *safe_area_insets)(neui_session_t session, neui_widget_t frame,
                                    int* left, int* top, int* right, int* bottom);
} neui_metrics_api_t;

#ifdef __cplusplus
}
#endif
