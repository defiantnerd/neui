#pragma once

#include <stdint.h>
#include "api.h"
#include "events.h"

#ifdef __cplusplus
extern "C" {
#endif

// Programmatic control over a scrolling SECTION. Wheel + scrollbar drag
// are wired automatically; this interface adds direct offset writes and
// a helper for scrolling a descendant widget into view.
//
// All offsets are in logical pixels at 96 DPI - same coordinate system as
// the SECTION's child layout. Both calls fire NEUI_EVENT_SCROLL_CHANGED
// when the committed offset actually moves.
#define NEUI_API_SCROLL "com.defiantnerd.neui.extension.scroll/0"

typedef struct neui_scroll_api
{
  uint32_t neui_version;

  // Set the absolute scroll offset on a scrolling SECTION. Values are
  // clamped to the section's legal scroll range ([0, content - body] per
  // axis). Cancels any in-flight rubber-band spring-back and resyncs the
  // kinetics integrator so a subsequent wheel event starts from the new
  // position. Returns 1 if the call applied (widget is a scrolling
  // SECTION); 0 otherwise (wrong widget kind, no scroll state allocated,
  // or cross-session handle). Fires NEUI_EVENT_SCROLL_CHANGED iff the
  // committed offset changed.
  int (NEUI_ABI *set_scroll)(neui_session_t session, neui_widget_t section,
                              int scroll_x, int scroll_y);

  // Read the current scroll offset of a scrolling SECTION. Returns 1 on
  // success (out_x / out_y filled), 0 otherwise. Either out pointer may
  // be NULL to skip that axis.
  int (NEUI_ABI *get_scroll)(neui_session_t session, neui_widget_t section,
                              int* out_x, int* out_y);

  // Scroll the nearest scrolling-SECTION ancestor so the given widget is
  // fully visible inside that section's body rect. Walks up the parent
  // chain until it finds a SECTION with NEUI_ATTR_SCROLL_MODE != "none";
  // returns 0 (no-op) if no scrolling ancestor exists. Minimum-motion
  // policy: a widget already fully visible is left alone; widgets above
  // the viewport scroll up just enough to bring their top into view;
  // widgets below scroll down just enough to bring their bottom in.
  // Widgets larger than the body align to the top-left. Fires
  // NEUI_EVENT_SCROLL_CHANGED iff the offset moved.
  int (NEUI_ABI *ensure_visible)(neui_session_t session, neui_widget_t widget);
} neui_scroll_api_t;

#ifdef __cplusplus
}
#endif
