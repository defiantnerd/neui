#pragma once

#include <stdint.h>
#include <stdbool.h>
#include "api.h"
#include "events.h"      // neui_event_dnd_t, neui_data_item_t fwd
#include "clipboard.h"   // shared MIME constants (NEUI_MIME_*)

#ifdef __cplusplus
extern "C" {
#endif

// Drag&drop API. v1 ships drop-target support only: widgets can receive
// drags from external apps (Explorer, Finder, browsers, ...). Initiating
// drags from inside the app is reserved for a future revision.
//
// A drop target is any widget with `set_drop_target(w, true)`. While a
// drag is in flight the framework hit-tests cursor positions through the
// widget tree (deepest first), finds the first drop_target widget whose
// accepted-MIMEs intersects the drag's advertised formats, and fires
// NEUI_EVENT_DND_ENTER / MOVE / LEAVE on that widget. The client decides
// per dispatch whether it will take the drag - and in which mode (COPY /
// MOVE / LINK) - by calling dnd_api->accept(session, action). Default is
// NEUI_DND_ACTION_NONE, which surfaces as the OS "no-drop" cursor.
//
// On NEUI_EVENT_DND_DROP the event carries a transient neui_data_item_t
// id. Use clipboard_api->item_get_format(item, mime, ...) to read bytes;
// the item is released the moment the callback returns, so copy out
// anything you want to keep.
//
// Coordinate space: x/y on the event payload are widget-local logical
// pixels, just like NEUI_EVENT_MOUSE_*.

// NEUI_API_DND is defined in d/api.h.

// Drag operations the source proposes and the target accepts. Bitmask so
// "copy or move, whichever the OS picks" is expressible. Numeric values
// align with Win32 DROPEFFECT_* (1=COPY, 2=MOVE, 4=LINK) so the platform
// layer forwards them with a small mask.
typedef enum neui_dnd_action {
  NEUI_DND_ACTION_NONE = 0,
  NEUI_DND_ACTION_COPY = 1,
  NEUI_DND_ACTION_MOVE = 2,
  NEUI_DND_ACTION_LINK = 4,
} neui_dnd_action_t;

typedef struct neui_dnd_api {
  uint32_t neui_version;

  // Mark a widget as a drop target. Any widget type is accepted (frames
  // act as catch-all when no descendant takes the drag). Default: false.
  void (NEUI_ABI *set_drop_target)(neui_session_t session,
                                    neui_widget_t widget,
                                    bool enable);

  // Read the current drop-target flag.
  bool (NEUI_ABI *get_drop_target)(neui_session_t session,
                                    neui_widget_t widget);

  // Restrict the MIME types this widget will accept. Pass count=0 (or
  // mimes=NULL) to clear - meaning "accept any". Stored per-widget; the
  // host copies the strings, so the caller's buffers can go away after
  // the call returns.
  void (NEUI_ABI *set_accepted_formats)(neui_session_t session,
                                         neui_widget_t widget,
                                         const char* const* mimes,
                                         int count);

  // Signal which action the client will accept for the current drag.
  // Call from inside the onevent callback during DND_ENTER / DND_MOVE
  // (and DND_DROP, where it determines the final reported effect).
  // The framework caches the value until the next dispatch and reports
  // it back to the OS pasteboard so the cursor reflects accept/reject.
  // Calling outside a DnD dispatch is a no-op.
  void (NEUI_ABI *accept)(neui_session_t session,
                           neui_dnd_action_t action);
} neui_dnd_api_t;

#ifdef __cplusplus
}
#endif
